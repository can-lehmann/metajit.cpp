#pragma once

// Copyright 2026 Can Joshua Lehmann
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


/*

Translation Validation for metajit.cpp

We use a provenance based memory model: Each pointer is a tuple (id, offset) where
id is a unique identifier for a memory region and offset is a byte offset into that region.
This means that casting an integer to a pointer is not supported as it would require
reconstructing the id.

*/


#include <z3++.h>

#include "jitir.hpp"

namespace metajit {
  namespace tv {
    class ValueState {
    private:
      Type _type;
      z3::expr _value;
      std::optional<z3::expr> _provenance;
    public:
      ValueState(Type type, z3::expr value): _type(type), _value(value) {}
      ValueState(Type type, z3::expr value, z3::expr provenance):
        _type(type), _value(value), _provenance(provenance) {}

      static ValueState invalid(z3::context& context) {
        return ValueState(Type::Bool, z3::expr(context));
      }
      
      Type type() const { return _type; }

      z3::expr value() const { return _value; }

      bool has_provenance() const { return _provenance.has_value(); }
      z3::expr provenance() const { return _provenance.value(); }
      z3::expr provenance_or(size_t width) const {
        if (_provenance.has_value()) {
          return _provenance.value();
        } else {
          return _value.ctx().bv_val(0, width);
        }
      }

      void set_provenance(z3::expr provenance) {
        _provenance = provenance;
      }

      void merge(const ValueState& other, z3::expr enable) {
        assert(_type == other._type);
        _value = z3::ite(enable, other._value, _value);
        if (has_provenance() || other.has_provenance()) {
          size_t provenance_width;
          if (has_provenance()) {
            provenance_width = provenance().get_sort().bv_size();
          } else {
            provenance_width = other.provenance().get_sort().bv_size();
          }

          _provenance = z3::ite(enable,
            other.provenance_or(provenance_width),
            provenance_or(provenance_width)
          );
        }
      }

      template <class Fn>
      ValueState map(Fn fn) const {
        ValueState result = *this;
        result._value = fn(_value);
        return result;
      }

      ValueState add_ptr(ValueState offset) const {
        assert(_type == Type::Ptr);
        return map([&](z3::expr value) {
          return value + offset.value();
        });
      }

      ValueState add_ptr(uint64_t offset) const {
        return add_ptr(ValueState(Type::Ptr, _value.ctx().bv_val(offset, type_width(Type::Ptr))));
      }

      ValueState eval(z3::model& model) {
        ValueState result = *this;
        result._value = model.eval(_value, true);
        if (has_provenance()) {
          result._provenance = model.eval(provenance(), true);
        }
        return result;
      }
    };

    class MemoryState {
    private:
      static inline z3::expr ptr_array(z3::context& context,
                                       const std::string& name,
                                       size_t width) {
        z3::sort sort = context.array_sort(
          context.bv_sort(type_width(Type::Ptr)),
          context.bv_sort(width)
        );
        return context.constant(name.c_str(), sort);
      }

      struct Region {
        // We use the theory of arrays to model memory.

        size_t id;
        z3::context* context;
        z3::expr bytes;
        z3::expr provenance; // For each byte we can additionally store a provenance id
        z3::expr size;

        Region(size_t _id, z3::expr _size, size_t _provenance_width):
          id(_id),
          context(&_size.ctx()),
          bytes(ptr_array(*context, "region" + std::to_string(_id) + "_bytes", 8)),
          provenance(ptr_array(*context, "region" + std::to_string(_id) + "_provenance", _provenance_width)),
          size(_size) {}
        
        Region(z3::context& context, size_t _id, size_t _provenance_width):
          Region(
            _id,
            context.bv_const(
              ("region" + std::to_string(_id) + "_size").c_str(),
              type_width(Type::Ptr)
            ),
            _provenance_width
          ) {}
        
        void merge(const Region& other, z3::expr enable) {
          assert(id == other.id);
          bytes = z3::ite(enable, other.bytes, bytes);
          provenance = z3::ite(enable, other.provenance, provenance);
          size = z3::ite(enable, other.size, size);
        }

        ValueState load(z3::expr offset, Type type) {
          z3::expr data = z3::select(bytes, offset);
          for (size_t it = 1; it < type_size(type); it++) {
            z3::expr byte_offset = context->bv_val(it, type_width(Type::Ptr));
            data = z3::concat(z3::select(bytes, offset + byte_offset), data);
          }
          z3::expr prov = z3::select(provenance, offset);
          return ValueState(type, data, prov);
        }

        void store(z3::expr offset, ValueState value, z3::expr enable) {
          assert(value.value().get_sort().bv_size() % 8 == 0);

          z3::expr new_bytes = bytes;
          z3::expr new_provenance = provenance;
          for (size_t it = 0; it < type_size(value.type()); it++) {
            z3::expr byte = value.value().extract((it + 1) * 8 - 1, it * 8);
            assert(byte.get_sort().bv_size() == 8);

            z3::expr byte_offset = context->bv_val(it, type_width(Type::Ptr));
            new_bytes = z3::store(new_bytes, offset + byte_offset, byte);

            if (value.has_provenance()) {
              z3::expr prov_byte = value.provenance();
              new_provenance = z3::store(new_provenance, offset + byte_offset, prov_byte);
            }
          }
          bytes = z3::ite(enable, new_bytes, bytes);
          provenance = z3::ite(enable, new_provenance, provenance);
        }
      };

      z3::context* _context;
      size_t _provenance_width = 1;
      std::vector<Region> _regions;
    public:
      MemoryState(z3::context& context, std::vector<std::optional<size_t>> regions): _context(&context) {
        while (1 << _provenance_width < regions.size()) {
          assert(_provenance_width < sizeof(size_t) * 8);
          _provenance_width++;
        }

        for (size_t id = 0; id < regions.size(); id++) {
          std::optional<size_t> size = regions[id];
          if (size.has_value()) {
            z3::expr size_expr = context.bv_val(size.value(), type_width(Type::Ptr));
            _regions.emplace_back(id, size_expr, _provenance_width);
          } else {
            _regions.emplace_back(context, id, _provenance_width);
          }
        }
      }

      ValueState load(ValueState pointer, Type type) {
        assert(pointer.has_provenance());

        std::optional<ValueState> result;
        for (Region region : _regions) {
          z3::expr in_region = pointer.provenance() == _context->bv_val(region.id, _provenance_width);
          ValueState value = region.load(pointer.value(), type);
          if (result.has_value()) {
            result->merge(value, in_region);
          } else {
            result = value;
          }
        }

        return result.value_or(ValueState::invalid(*_context));
      }

      void store(ValueState pointer, ValueState value) {
        assert(pointer.has_provenance());

        for (Region& region : _regions) {
          z3::expr in_region = pointer.provenance() == _context->bv_val(region.id, _provenance_width);
          region.store(pointer.value(), value, in_region);
        } 
      }

      void merge(const MemoryState& other, z3::expr enable) {
        assert(_regions.size() == other._regions.size());
        for (size_t it = 0; it < _regions.size(); it++) {
          _regions[it].merge(other._regions[it], enable);
        }
      }
    };

    class Z3CodeGen: public Pass<Z3CodeGen> {
    private:
      Section* _section;
      z3::context& _context;

      struct BlockData {
        z3::expr active;
        std::vector<ValueState> args;
        std::optional<MemoryState> memory_state;

        BlockData(z3::context& context, Block* block):
            active(context.bool_val(false)) {
          
          if (block) {
            for (Arg* arg : block->args()) {
              args.push_back(ValueState(arg->type(), context.bv_val(0, type_width(arg->type()))));
            }
          }
        }
      };

      std::unordered_map<Block*, BlockData> _blocks;
      std::unordered_map<NamedValue*, ValueState> _values;

    public:
      ValueState emit(Value* value) {
        if (dynmatch(Const, constant, value)) {
          return ValueState(
            constant->type(),
            _context.bv_val(constant->value(), type_width(constant->type()))
          );
        } else if (value->is_named()) {
          return _values.at((NamedValue*) value);
        } else {
          assert(false && "Unknown value");
          return ValueState::invalid(_context);
        }
      }

      z3::expr bool2bit(z3::expr cond) {
        return z3::ite(cond, _context.bv_val(1, 1), _context.bv_val(0, 1));
      }

    private:
      z3::expr resize(z3::expr value, size_t new_width, bool is_signed) {
        size_t old_width = value.get_sort().bv_size();
        if (new_width == old_width) {
          return value;
        } else if (new_width < old_width) {
          return value.extract(new_width - 1, 0);
        } else {
          if (is_signed) {
            return z3::sext(value, new_width);
          } else {
            return z3::zext(value, new_width);
          }
        }
      }

      ValueState resize(ValueState value, Type to_type, bool is_signed) {
        ValueState result(to_type, resize(value.value(), type_width(to_type), is_signed));
        if (value.has_provenance()) {
          result.set_provenance(value.provenance());
        }
        return result;
      }

      void enter(Block* block, Block* from, z3::expr enable, std::vector<ValueState> args) {
        assert(enable.get_sort().is_bool());
        z3::expr taken = enable && _blocks.at(from).active;
        BlockData& block_data = _blocks.at(block);
        block_data.active = block_data.active || taken;
        for (size_t it = 0; it < args.size(); it++) {
          block_data.args[it].merge(args[it], taken);
        }
      }

      MemoryState& memory_state(Block* block) {
        BlockData& block_data = _blocks.at(block);
        assert(block_data.memory_state.has_value());
        return block_data.memory_state.value();
      }

      ValueState emit_inst(Inst* inst, Block* block) {
        if (dynamic_cast<FreezeInst*>(inst) || dynamic_cast<AssumeConstInst*>(inst)) {
          return emit(inst->arg(0));
        } else if (dynmatch(SelectInst, select, inst)) {
          z3::expr cond = emit(select->arg(0)).value().bit2bool(0);
          ValueState true_val = emit(select->arg(1));
          ValueState false_val = emit(select->arg(2));
          false_val.merge(true_val, cond);
          return false_val;
        } else if (dynmatch(ResizeUInst, resize_u, inst)) {
          return resize(emit(resize_u->arg(0)), resize_u->type(), false);
        } else if (dynmatch(ResizeSInst, resize_s, inst)) {
          return resize(emit(resize_s->arg(0)), resize_s->type(), true);
        } else if (dynmatch(ResizeXInst, resize_x, inst)) {
          return resize(emit(resize_x->arg(0)), resize_x->type(), false);
        } else if (dynmatch(LoadInst, load, inst)) {
          ValueState pointer = emit(load->arg(0)).add_ptr(load->offset());
          return memory_state(block).load(pointer, load->type());
        } else if (dynmatch(StoreInst, store, inst)) {
          ValueState pointer = emit(store->arg(0)).add_ptr(store->offset());
          memory_state(block).store(pointer, emit(store->arg(1)));
        } else if (dynmatch(AddPtrInst, add_ptr, inst)) {
          ValueState pointer = emit(add_ptr->arg(0));
          ValueState offset = emit(add_ptr->arg(1));
          return pointer.add_ptr(offset);
        }

        #define binop(InstType, expression) \
          else if (dynamic_cast<InstType*>(inst)) { \
            z3::expr a = emit(inst->arg(0)).value(); \
            z3::expr b = emit(inst->arg(1)).value(); \
            return ValueState(inst->type(), expression); \
          }
        
        binop(AddInst, a + b)
        binop(SubInst, a - b)
        binop(MulInst, a * b)
        binop(DivSInst, z3::sdiv(a, b))
        binop(DivUInst, z3::udiv(a, b))
        binop(ModSInst, z3::srem(a, b))
        binop(ModUInst, z3::urem(a, b))
        binop(AndInst, a & b)
        binop(OrInst, a | b)
        binop(XorInst, a ^ b)
        binop(ShlInst, z3::shl(a, b))
        binop(ShrUInst, z3::lshr(a, b))
        binop(ShrSInst, z3::ashr(a, b))
        binop(EqInst, bool2bit(a == b))
        binop(LtUInst, bool2bit(z3::ult(a, b)))
        binop(LtSInst, bool2bit(z3::slt(a, b)))

        #undef binop

        else if (dynmatch(BranchInst, branch, inst)) {
          z3::expr cond = emit(branch->arg(0)).value().bit2bool(0);
          enter(branch->true_block(), block, cond, {});
          enter(branch->false_block(), block, !cond, {});
        } else if (dynmatch(JumpInst, jump, inst)) {
          std::vector<ValueState> args;
          for (Value* arg : jump->args()) {
            args.push_back(emit(arg));
          }
          enter(jump->block(), block, _context.bool_val(true), args);
        } else if (dynmatch(ExitInst, exit, inst)) {
          enter(nullptr, block, _context.bool_val(true), {});
        } else if (dynmatch(CommentInst, comment, inst)) {
        } else {
          assert(false && "Unknown instruction");
        }

        return ValueState::invalid(_context);
      }

      void emit_block(Block* block) {
        for (Arg* arg : block->args()) {
          _values.emplace(arg, _blocks.at(block).args[arg->index()]);
        }
        for (Inst* inst : *block) {
          _values.emplace(inst, emit_inst(inst, block));
        }
      }
    public:
      Z3CodeGen(Section* section,
                z3::context& context,
                std::vector<ValueState> entry_args,
                MemoryState entry_memory_state):
          Pass<Z3CodeGen>(section),
          _section(section),
          _context(context) {
        
        for (Block* block : *section) {
          _blocks.emplace(block, BlockData(context, block));
        }

        _blocks.emplace(nullptr, BlockData(context, nullptr)); // For exit block

        assert(entry_args.size() == section->entry()->args().size());

        _blocks.at(section->entry()).active = _context.bool_val(true);
        _blocks.at(section->entry()).args = entry_args;
        _blocks.at(section->entry()).memory_state = entry_memory_state;
        
        for (Block* block : *section) {
          emit_block(block);
        }
      }

    };
  }
}
