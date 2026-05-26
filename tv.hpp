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
      z3::expr _is_poison;
      std::optional<z3::expr> _provenance;
    public:
      ValueState(Type type, z3::expr value):
        _type(type), _value(value), _is_poison(value.ctx().bool_val(false)) {}
      ValueState(Type type, z3::expr value, z3::expr provenance):
        _type(type), _value(value), _is_poison(value.ctx().bool_val(false)),
        _provenance(provenance) {}

      static ValueState invalid(z3::context& context) {
        return ValueState(Type::Bool, z3::expr(context));
      }

      Type type() const { return _type; }

      z3::expr value() const { return _value; }

      z3::expr is_poison() const { return _is_poison; }
      void set_poison(z3::expr is_poison) { _is_poison = is_poison; }

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
        _is_poison = z3::ite(enable, other._is_poison, _is_poison);
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
        result._is_poison = model.eval(_is_poison, true);
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
          // Truncate to type_width bits (handles Bool: 8 stored bits -> 1 bit)
          if (type_width(type) < type_size(type) * 8) {
            data = data.extract(type_width(type) - 1, 0);
          }
          z3::expr prov = z3::select(provenance, offset);
          return ValueState(type, data, prov);
        }

        void store(z3::expr offset, ValueState value, z3::expr enable) {
          // Zero-extend to type_size*8 bits if needed (handles Bool: 1 bit -> 8 stored bits)
          z3::expr stored = value.value();
          size_t stored_bits = type_size(value.type()) * 8;
          if (stored.get_sort().bv_size() < stored_bits) {
            stored = z3::zext(stored, stored_bits - stored.get_sort().bv_size());
          }
          assert(stored.get_sort().bv_size() % 8 == 0);

          z3::expr new_bytes = bytes;
          z3::expr new_provenance = provenance;
          for (size_t it = 0; it < type_size(value.type()); it++) {
            z3::expr byte = stored.extract((it + 1) * 8 - 1, it * 8);
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

      size_t provenance_width() const { return _provenance_width; }

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
        z3::expr ub;
        std::vector<ValueState> args;
        std::optional<MemoryState> memory_state;

        BlockData(z3::context& context, Block* block):
            active(context.bool_val(false)),
            ub(context.bool_val(false)) {

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
        } else if (dynmatch(Poison, poison, value)) {
          ValueState result(
            poison->type(),
            _context.bv_val(0, type_width(poison->type()))
          );
          result.set_poison(_context.bool_val(true));
          return result;
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
            return z3::sext(value, new_width - old_width);
          } else {
            return z3::zext(value, new_width - old_width);
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

        if (block_data.memory_state.has_value()) {
          block_data.memory_state->merge(memory_state(from), taken);
        } else {
          block_data.memory_state = memory_state(from);
        }
      }

      MemoryState& memory_state(Block* block) {
        BlockData& block_data = _blocks.at(block);
        assert(block_data.memory_state.has_value());
        return block_data.memory_state.value();
      }

      // Returns the OR of is_poison() for all value arguments of inst.
      z3::expr input_poison(Inst* inst) {
        z3::expr result = _context.bool_val(false);
        for (size_t it = 0; it < inst->arg_count(); it++) {
          result = result || emit(inst->arg(it)).is_poison();
        }
        return result;
      }

      ValueState emit_inst(Inst* inst, Block* block) {
        if (dynamic_cast<FreezeInst*>(inst) || dynamic_cast<AssumeConstInst*>(inst)) {
          return emit(inst->arg(0));
        } else if (dynmatch(SelectInst, select, inst)) {
          ValueState cond_state = emit(select->arg(0));
          ValueState true_val = emit(select->arg(1));
          ValueState false_val = emit(select->arg(2));
          z3::expr cond = cond_state.value().bit2bool(0);
          // Value: pick based on condition
          ValueState result(select->type(), z3::ite(cond, true_val.value(), false_val.value()));
          // Poison: condition is poison, OR the selected branch is poison
          result.set_poison(cond_state.is_poison() ||
                            z3::ite(cond, true_val.is_poison(), false_val.is_poison()));
          return result;
        } else if (dynmatch(ResizeUInst, resize_u, inst)) {
          ValueState result = resize(emit(resize_u->arg(0)), resize_u->type(), false);
          result.set_poison(input_poison(inst));
          return result;
        } else if (dynmatch(ResizeSInst, resize_s, inst)) {
          ValueState result = resize(emit(resize_s->arg(0)), resize_s->type(), true);
          result.set_poison(input_poison(inst));
          return result;
        } else if (dynmatch(ResizeXInst, resize_x, inst)) {
          ValueState result = resize(emit(resize_x->arg(0)), resize_x->type(), false);
          result.set_poison(input_poison(inst));
          return result;
        } else if (dynmatch(LoadInst, load, inst)) {
          ValueState ptr_state = emit(load->arg(0));
          _blocks.at(block).ub = _blocks.at(block).ub ||
            (_blocks.at(block).active && ptr_state.is_poison());
          if (ptr_state.has_provenance()) {
            ValueState pointer = ptr_state.add_ptr(load->offset());
            return memory_state(block).load(pointer, load->type());
          }
          return ValueState(load->type(), _context.bv_val(0, type_width(load->type())));
        } else if (dynmatch(StoreInst, store, inst)) {
          ValueState ptr_state = emit(store->arg(0));
          ValueState val_state = emit(store->arg(1));
          _blocks.at(block).ub = _blocks.at(block).ub ||
            (_blocks.at(block).active &&
             (ptr_state.is_poison() || val_state.is_poison()));
          if (ptr_state.has_provenance()) {
            ValueState pointer = ptr_state.add_ptr(store->offset());
            memory_state(block).store(pointer, val_state);
          }
        } else if (dynmatch(AddPtrInst, add_ptr, inst)) {
          ValueState pointer = emit(add_ptr->arg(0));
          ValueState offset = emit(add_ptr->arg(1));
          ValueState result = pointer.add_ptr(offset);
          result.set_poison(input_poison(inst));
          return result;
        }

        #define binop(InstType, expression) \
          else if (dynamic_cast<InstType*>(inst)) { \
            z3::expr a = emit(inst->arg(0)).value(); \
            z3::expr b = emit(inst->arg(1)).value(); \
            ValueState result(inst->type(), expression); \
            result.set_poison(input_poison(inst)); \
            return result; \
          }

        #define binop_divmod(InstType, expression) \
          else if (dynamic_cast<InstType*>(inst)) { \
            z3::expr a = emit(inst->arg(0)).value(); \
            z3::expr b = emit(inst->arg(1)).value(); \
            ValueState result(inst->type(), expression); \
            result.set_poison(input_poison(inst) || \
                              b == _context.bv_val(0, type_width(inst->type()))); \
            return result; \
          }

        #define binop_shift(InstType, expression) \
          else if (dynamic_cast<InstType*>(inst)) { \
            z3::expr a = emit(inst->arg(0)).value(); \
            z3::expr b = emit(inst->arg(1)).value(); \
            ValueState result(inst->type(), expression); \
            result.set_poison(input_poison(inst) || \
                              z3::uge(b, _context.bv_val(type_width(inst->type()), type_width(inst->type())))); \
            return result; \
          }

        binop(AddInst, a + b)
        binop(SubInst, a - b)
        binop(MulInst, a * b)
        binop_divmod(DivSInst, a / b)
        binop_divmod(DivUInst, z3::udiv(a, b))
        binop_divmod(ModSInst, z3::srem(a, b))
        binop_divmod(ModUInst, z3::urem(a, b))
        binop(AndInst, a & b)
        binop(OrInst, a | b)
        binop(XorInst, a ^ b)
        binop_shift(ShlInst, z3::shl(a, b))
        binop_shift(ShrUInst, z3::lshr(a, b))
        binop_shift(ShrSInst, z3::ashr(a, b))
        binop(EqInst, bool2bit(a == b))
        binop(LtUInst, bool2bit(z3::ult(a, b)))
        binop(LtSInst, bool2bit(z3::slt(a, b)))

        #undef binop
        #undef binop_divmod
        #undef binop_shift

        else if (dynmatch(BranchInst, branch, inst)) {
          ValueState cond_state = emit(branch->arg(0));
          z3::expr cond = cond_state.value().bit2bool(0);
          _blocks.at(block).ub = _blocks.at(block).ub ||
            (_blocks.at(block).active && cond_state.is_poison());
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
      // Returns a Z3 boolean that is true iff any reachable path through the
      // section hits undefined behavior (branching/storing/loading on poison).
      z3::expr has_ub() const {
        z3::expr result = _context.bool_val(false);
        for (const auto& [block, data] : _blocks) {
          result = result || data.ub;
        }
        return result;
      }

      // Returns the symbolic memory state at the exit point of the section.
      MemoryState& exit_memory_state() {
        return memory_state(nullptr);
      }

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

    // TVTestData: inputs are section entry args, outputs are stored to a heap pointer
    // (also an entry arg). This avoids routing inputs through memory, keeping Z3
    // queries simple, while still forcing outputs to be live (not DCE'd).
    class TVTestData {
    public:
      struct Output {
        size_t offset = 0;
        Type type = Type::Void;
      };
    private:
      Builder* _builder = nullptr;
      Arg* _data = nullptr;        // entry arg 0: Ptr to output buffer
      size_t _data_size = 0;
      std::vector<Output> _outputs;
      std::vector<Type> _entry_arg_types; // types of input entry args (not including _data)

      size_t alloc(Type type) {
        if (_data_size % type_size(type) != 0) {
          _data_size += type_size(type) - (_data_size % type_size(type));
        }
        size_t offset = _data_size;
        _data_size += type_size(type);
        return offset;
      }
    public:
      TVTestData() {}
      TVTestData(Builder& builder, std::vector<Type> input_types):
          _builder(&builder) {
        // Entry args: [Ptr (output buffer), input0, input1, ...]
        std::vector<Type> entry_types = {Type::Ptr};
        for (Type t : input_types) {
          entry_types.push_back(t);
        }
        _entry_arg_types = input_types;
        builder.move_to_end(builder.build_block(entry_types));
        _data = builder.entry_arg(0);
      }

      // Returns the i-th input as a Value* (directly an entry arg).
      Value* input(size_t index) {
        assert(_builder);
        return _builder->entry_arg(1 + index);
      }

      // Records value as an output: stores it into the output buffer.
      void output(Value* value) {
        assert(_builder && _data);
        size_t offset = alloc(value->type());
        _outputs.push_back({offset, value->type()});
        _builder->build_store(_data, value, AliasingGroup(0), offset);
      }

      size_t data_size() const { return _data_size; }
      const std::vector<Output>& outputs() const { return _outputs; }
      const std::vector<Type>& entry_arg_types() const { return _entry_arg_types; }
    };

    // Check that `after` refines `before` for all outputs recorded in `data`.
    // Uses symbolic execution via Z3CodeGen. If a counterexample is found,
    // throws a std::runtime_error with a description.
    inline void check_tv_refinement(Section* before,
                                    Section* after,
                                    const TVTestData& data) {
      z3::context z3_context;

      // One output region (the data buffer), size unknown
      std::vector<std::optional<size_t>> regions = {std::nullopt};
      MemoryState before_memory(z3_context, regions);
      MemoryState after_memory(z3_context, regions);

      // Build symbolic entry args: data pointer (with provenance 0) + inputs
      std::vector<ValueState> entry_args;

      ValueState data_ptr(Type::Ptr, z3_context.bv_const("data_ptr", type_width(Type::Ptr)));
      data_ptr.set_provenance(z3_context.bv_val(0, before_memory.provenance_width()));
      entry_args.push_back(data_ptr);

      for (size_t it = 0; it < data.entry_arg_types().size(); it++) {
        Type type = data.entry_arg_types()[it];
        entry_args.push_back(ValueState(
          type,
          z3_context.bv_const(("input" + std::to_string(it)).c_str(), type_width(type))
        ));
      }

      Z3CodeGen before_cg(before, z3_context, entry_args, before_memory);
      Z3CodeGen after_cg(after, z3_context, entry_args, after_memory);

      z3::solver solver(z3_context);
      solver.set("timeout", (unsigned) 5000); // 5 second timeout per query

      // For each output slot, check refinement:
      // before is NOT UB AND before result is NOT poison AND
      // (after result IS poison OR values differ)
      for (const TVTestData::Output& output : data.outputs()) {
        ValueState ptr(Type::Ptr,
          data_ptr.value() + z3_context.bv_val(output.offset, type_width(Type::Ptr)),
          data_ptr.provenance()
        );

        ValueState before_val = before_cg.exit_memory_state().load(ptr, output.type);
        ValueState after_val  = after_cg.exit_memory_state().load(ptr, output.type);

        z3::expr counterexample =
          !before_cg.has_ub() &&
          !before_val.is_poison() &&
          (after_val.is_poison() || before_val.value() != after_val.value());

        solver.add(counterexample.simplify());
      }

      z3::check_result result = solver.check();
      if (result == z3::sat) {
        z3::model model = solver.get_model();
        std::ostringstream stream;
        stream << "TV refinement counterexample found\n";
        stream << "Inputs:\n";
        for (size_t it = 0; it < data.entry_arg_types().size(); it++) {
          ValueState arg = before_cg.emit(before->entry()->arg(1 + it)).eval(model);
          stream << "  input" << it << " = " << arg.value() << "\n";
        }
        stream << "Outputs:\n";
        for (const TVTestData::Output& output : data.outputs()) {
          ValueState ptr(Type::Ptr,
            data_ptr.value() + z3_context.bv_val(output.offset, type_width(Type::Ptr)),
            data_ptr.provenance()
          );
          ValueState before_val = before_cg.exit_memory_state().load(ptr, output.type).eval(model);
          ValueState after_val  = after_cg.exit_memory_state().load(ptr, output.type).eval(model);
          stream << "  before: " << before_val.value()
                 << " (poison=" << model.eval(before_val.is_poison(), true) << ")\n";
          stream << "  after:  " << after_val.value()
                 << " (poison=" << model.eval(after_val.is_poison(), true) << ")\n";
        }
        throw std::runtime_error(stream.str());
      } else if (result == z3::unknown) {
        // Timeout or resource limit reached — skip this check
      } else if (result != z3::unsat) {
        throw std::runtime_error("check_tv_refinement: Z3 failed to determine satisfiability");
      }
    }
  }
}
