#pragma once

// Copyright 2025 Can Joshua Lehmann
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

#include <variant>
#include <fstream>

#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include "jitir.hpp"

namespace metajit {
  class Reg {
  public:
    enum class Kind {
      Invalid, Virtual, Physical
    };
  private:
    Kind _kind = Kind::Invalid;
    size_t _id = 0;
  public:
    constexpr Reg() {}
    constexpr Reg(Kind kind, size_t id): _kind(kind), _id(id) {}

    static constexpr Reg phys(size_t id) {
      return Reg(Kind::Physical, id);
    }

    static constexpr Reg virt(size_t id) {
      return Reg(Kind::Virtual, id);
    }

    Kind kind() const { return _kind; }
    size_t id() const { return _id; }

    bool is_invalid() const { return _kind == Kind::Invalid; }
    bool is_virtual() const { return _kind == Kind::Virtual; }
    bool is_physical() const { return _kind == Kind::Physical; }

    bool operator==(const Reg& other) const {
      return _id == other._id;
    }

    bool operator!=(const Reg& other) const {
      return !(*this == other);
    }

    void write(std::ostream& stream) const {
      switch (_kind) {
        case Kind::Invalid:
          stream << "<INVALID>";
        break;
        case Kind::Virtual:
          stream << "v" << _id;
        break;
        case Kind::Physical:
          stream << "p" << _id;
        break;
      }
    }
  };
}

std::ostream& operator<<(std::ostream& stream, const metajit::Reg& reg) {
  reg.write(stream);
  return stream;
}

namespace metajit {
  class X86Block;

  class X86Inst: public lwir::LinkedListItem<X86Inst> {
  public:
    enum class Kind {
      #define x86_inst(name, ...) name,
      #include "x86insts.inc.hpp"
    };

    struct Mem {
      Reg base;
      size_t scale = 0;
      Reg index;
      int32_t disp = 0;

      Mem() {}
      Mem(Reg _base): base(_base) {}
      Mem(Reg _base, int32_t _disp): base(_base), disp(_disp) {}
      Mem(Reg _base, size_t _scale, Reg _index, int32_t _disp):
        base(_base), scale(_scale), index(_index), disp(_disp) {}

      void write(std::ostream& stream) const {
        stream << "[" << base;
        if (scale != 0) {
          stream << " + " << index << " * " << scale;
        }
        if (disp != 0) {
          stream << " + " << disp;
        }
        stream << "]";
      }

      bool is_invalid() const {
        return base.is_invalid();
      }
    };

    using RM = std::variant<std::monostate, Reg, Mem>;
    using Imm = std::variant<std::monostate, uint64_t, X86Block*>;
  private:
    Kind _kind;
    Reg _reg;
    RM _rm;
    Imm _imm;
    size_t _name = 0;
  public:
    X86Inst(Kind kind): _kind(kind) {}
    
    Kind kind() const { return _kind; }
    Reg reg() const { return _reg; }
    RM rm() const { return _rm; }
    Imm imm() const { return _imm; }

    X86Inst& set_kind(Kind kind) { _kind = kind; return *this; }
    X86Inst& set_reg(Reg reg) { _reg = reg; return *this; }
    X86Inst& set_rm(const RM& rm) { _rm = rm; return *this; }
    X86Inst& set_imm(const Imm& imm) { _imm = imm; return *this; }

    size_t name() const { return _name; }
    void set_name(size_t name) { _name = name; }

    bool is_64_bit() const {
      switch (_kind) {
        #define x86_inst(name, lowercase, usedef, is_64_bit, ...) \
          case Kind::name: return is_64_bit;
        #include "x86insts.inc.hpp"
      }
      return false;
    }

    template <class Fn>
    void visit_regs(const Fn& fn) {
      if (!_reg.is_invalid()) {
        fn(_reg);
      }

      if (std::holds_alternative<Reg>(_rm)) {
        fn(std::get<Reg>(_rm));
      } else if (std::holds_alternative<Mem>(_rm)) {
        Mem& mem = std::get<Mem>(_rm);
        fn(mem.base);
        if (!mem.index.is_invalid()) {
          fn(mem.index);
        }
      }
    }

    template <class UseFn, class DefFn>
    void visit_use_then_def(const UseFn& use_fn, const DefFn& def_fn) const {
      RM reg = std::monostate();
      if (!_reg.is_invalid()) {
        reg = _reg;
      }
      RM rm = _rm;

      auto use = [&](RM rm) {
        if (std::holds_alternative<Reg>(rm)) {
          use_fn(std::get<Reg>(rm));
        } else if (std::holds_alternative<Mem>(rm)) {
          Mem mem = std::get<Mem>(rm);
          use_fn(mem.base);
          if (!mem.index.is_invalid()) {
            use_fn(mem.index);
          }
        }
      };

      auto def = [&](RM rm) {
        if (std::holds_alternative<Reg>(rm)) {
          def_fn(std::get<Reg>(rm));
        } else {
          use(rm);
        }
      };

      switch (_kind) {
        #define x86_inst(name, lowercase, usedef, ...) \
          case Kind::name: usedef; break;
        #include "x86insts.inc.hpp"
      }
    }

    void write(std::ostream& stream) const;
  };

  class X86Block {
  private:
    lwir::LinkedList<X86Inst> _insts;
    X86Block* _loop = nullptr;
    Reg* _regalloc = nullptr;
    size_t _name = 0;
  public:
    X86Block() {}

    size_t name() const { return _name; }
    void set_name(size_t name) { _name = name; }

    Reg* regalloc() const { return _regalloc; }
    void set_regalloc(Reg* regalloc) { _regalloc = regalloc; }

    lwir::LinkedList<X86Inst>& insts() { return _insts; }

    auto begin() { return _insts.begin(); }
    auto end() { return _insts.end(); }

    X86Inst* first() { return _insts.first(); }
    X86Inst* last() { return _insts.last(); }

    auto rev_range() { return _insts.rev_range(); }

    X86Block* loop() const { return _loop; }
    void set_loop(X86Block* loop) { _loop = loop; }

    void add_incoming(X86Block* from) {
      if (from->name() >= _name) {
        if (!_loop || from->name() > _loop->name()) {
          _loop = from;
        }
      }
    }

    void insert_before(X86Inst* before, X86Inst* inst) {
      _insts.insert_before(before, inst);
    }

    void write(std::ostream& stream) {
      stream << "b" << _name;
      if (_loop) {
        stream << " loop until b" << _loop->name();
      }
      stream << ":\n";
      for (X86Inst* inst : _insts) {
        stream << "  ";
        inst->write(stream);
        stream << '\n';        
      }
    }
  };

  void X86Inst::write(std::ostream& stream) const {
    switch (_kind) {
      #define x86_inst(name, lowercase, ...) \
        case Kind::name: stream << #lowercase; break;
      #include "x86insts.inc.hpp"
    }

    if (!_reg.is_invalid()) {
      stream << " reg=" << _reg;
    }

    if (std::holds_alternative<Reg>(_rm)) {
      stream << " rm=" << std::get<Reg>(_rm);
    } else if (std::holds_alternative<Mem>(_rm)) {
      Mem mem = std::get<Mem>(_rm);
      stream << " rm=";
      mem.write(stream);
    }

    if (std::holds_alternative<uint64_t>(_imm)) {
      stream << " imm=" << std::get<uint64_t>(_imm);
    } else if (std::holds_alternative<X86Block*>(_imm)) {
      stream << " imm=b" << std::get<X86Block*>(_imm)->name();
    }
  }

  class X86InstBuilder {
  private:
    Allocator& _allocator;
    X86Block* _block = nullptr;
    X86Inst* _insert_pos = nullptr;

    X86Inst& build(X86Inst::Kind kind) {
      X86Inst* inst = (X86Inst*) _allocator.alloc(sizeof(X86Inst), alignof(X86Inst));
      new (inst) X86Inst(kind);
      _block->insert_before(_insert_pos, inst);
      return *inst;
    }
  public:
    X86InstBuilder(Allocator& allocator, X86Block* block):
      _allocator(allocator),
      _block(block),
      _insert_pos(nullptr) {}
    
    X86Block* block() const { return _block; }
    void set_block(X86Block* block) { _block = block; }

    void move_before(X86Block* block, X86Inst* inst) {
      _block = block;
      _insert_pos = inst;
    }

    void move_to_begin(X86Block* block) {
      _block = block;
      _insert_pos = block->first();
    }

    X86Block* build_block() {
      X86Block* block = (X86Block*) _allocator.alloc(sizeof(X86Block), alignof(X86Block));
      new (block) X86Block();
      return block;
    }

    #define binop_x86_inst(kind, name, ...) \
      X86Inst* name(Reg dst, X86Inst::RM src) { \
        return &build(X86Inst::Kind::kind).set_reg(dst).set_rm(src); \
      }
    
    #define rev_binop_x86_inst(kind, name, ...) \
      X86Inst* name(X86Inst::RM dst, Reg src) { \
        return &build(X86Inst::Kind::kind).set_rm(dst).set_reg(src); \
      }
    
    #define imm_binop_x86_inst(kind, name, ...) \
      X86Inst* name(X86Inst::RM dst, X86Inst::Imm imm) { \
        return &build(X86Inst::Kind::kind).set_rm(dst).set_imm(imm); \
      }

    #define jmp_x86_inst(kind, name, ...) \
      X86Inst* name(X86Block* imm) { \
        return &build(X86Inst::Kind::kind).set_imm(imm); \
      }

    #define unop_x86_inst(kind, name, ...) \
      X86Inst* name(X86Inst::RM rm) { \
        return &build(X86Inst::Kind::kind).set_rm(rm); \
      }
    
    #define op0_x86_inst(kind, name, ...) \
      X86Inst* name() { \
        return &build(X86Inst::Kind::kind); \
      }

    #include "x86insts.inc.hpp"

    X86Inst* mov64_imm64(Reg dst, X86Inst::Imm imm) {
      return &build(X86Inst::Kind::Mov64Imm64).set_rm(dst).set_imm(imm);
    }
    
    X86Inst* lea64(Reg dst, X86Inst::Mem src) {
      return &build(X86Inst::Kind::Lea64).set_reg(dst).set_rm(src);
    }
  };

  class X86CodeGen: public Pass<X86CodeGen> {
  private:
    Section* _section;
    Allocator& _allocator;
    std::vector<X86Block*> _blocks;
    X86InstBuilder _builder;

    NameMap<void*> _memory_deps;

    struct Interval {
      size_t min = 0;
      size_t max = 0;

      Interval(): min(~size_t(0)), max(0) {}

      bool empty() const {
        return max < min;
      }

      void incl(size_t value) {
        if (value < min) {
          min = value;
        }
        if (value > max) {
          max = value;
        }
      }

      void write(std::ostream& stream) const {
        if (empty()) {
          stream << "{}";
        } else {
          stream << "[" << min << "; " << max << "]";          
        }
      }
    };

    struct VRegInfo {
      Reg fixed;
      Interval interval;
      Reg current_reg;
      size_t stack_offset = 0;
    };

    NameMap<Reg> _vregs;
    std::vector<VRegInfo> _vreg_info;
    
    void memory_deps() {
      for (Block* block : *_section) {
        std::unordered_map<AliasingGroup, void*> last_store;
        for (Inst* inst : *block) {
          #define find_dep(inst) \
            void* dep = (void*) block; \
            if (last_store.find(inst->aliasing()) != last_store.end()) { \
              dep = (void*) last_store.at(inst->aliasing()); \
            } \
            _memory_deps[inst] = dep;
          
          if (dynmatch(LoadInst, load, inst)) {
            find_dep(load);
          } else if (dynmatch(StoreInst, store, inst)) {
            find_dep(store);
            last_store[store->aliasing()] = store;
          } else {
            _memory_deps[inst] = nullptr;
          }

          #undef find_dep
        }
      }
    }

    Reg vreg() {
      size_t id = _vreg_info.size();
      _vreg_info.emplace_back();
      return Reg::virt(id);
    }

    Reg fix_to_preg(Reg vreg, Reg preg) {
      assert(vreg.is_virtual());
      VRegInfo& info = _vreg_info[vreg.id()];
      info.fixed = preg;
      return vreg;
    }

    bool is_sext_imm32(Const* constant) {
      if (type_size(constant->type()) == 8) {
        uint64_t value = constant->value();
        return (value >> 31) == 0 || (value >> 31) == 0x1fffffffULL;
      } else {
        return true;
      }
    }

    Reg vreg(Value* value) {
      if (dynmatch(Const, constant, value)) {
        Reg reg = vreg();
        switch (type_size(constant->type())) {
          case 1: _builder.mov8_imm(reg, constant->value()); break;
          case 2: _builder.mov16_imm(reg, constant->value()); break;
          case 4: _builder.mov32_imm(reg, constant->value()); break;
          case 8:
            if (is_sext_imm32(constant)) {
              _builder.mov64_imm(reg, constant->value());
            } else {
              _builder.mov64_imm64(reg, constant->value());
            }
          break;
          default:
            assert(false && "Unsupported constant type");
        }
        return reg;
      } else if (value->is_named()) {
        NamedValue* named = (NamedValue*) value;
        if (_vregs.at(named).is_invalid()) {
          _vregs[named] = vreg();
        }
        return _vregs.at(named);
      } else {
        assert(false && "Unknown value");
        return Reg();
      }
    }

    void build_add(Reg dst, Value* a, Value* b) {
      X86Inst::Mem mem;
      if (dynmatch(Const, constant_b, b)) {
        if (is_sext_imm32(constant_b)) {
          mem = X86Inst::Mem(
            vreg(a),
            constant_b->value()
          );
        }
      } else if (dynmatch(MulInst, mul, b)) {
        if (dynmatch(Const, constant_scale, mul->arg(1))) {
          if (constant_scale->value() == 2 ||
              constant_scale->value() == 4 ||
              constant_scale->value() == 8) {
            mem = X86Inst::Mem(
              vreg(a),
              constant_scale->value(),
              vreg(mul->arg(0)),
              0
            );
          }
        }
      }

      if (mem.is_invalid()) {
        mem = X86Inst::Mem(
          vreg(a),
          1,
          vreg(b),
          0
        );
      }

      _builder.lea64(dst, mem);
    }

    void build_cmp(Value* a, Value* b) {
      if (dynmatch(Const, constant_b, b)) {
        if (is_sext_imm32(constant_b)) {
          switch (type_size(a->type())) {
            case 1: _builder.cmp8_imm(vreg(a), constant_b->value()); break;
            case 2: _builder.cmp16_imm(vreg(a), constant_b->value()); break;
            case 4: _builder.cmp32_imm(vreg(a), constant_b->value()); break;
            case 8: _builder.cmp64_imm(vreg(a), constant_b->value()); break;
            default: assert(false && "Unsupported comparison type");
          }
          return;
        }
      }

      switch (type_size(a->type())) {
        case 1: _builder.cmp8(vreg(a), vreg(b)); break;
        case 2: _builder.cmp16(vreg(a), vreg(b)); break;
        case 4: _builder.cmp32(vreg(a), vreg(b)); break;
        case 8: _builder.cmp64(vreg(a), vreg(b)); break;
        default: assert(false && "Unsupported comparison type");
      }
    }

    void build_cmov(Reg res, Value* cond, Reg then) {
      if (cond->is_inst()) {
        Inst* pred_inst = (Inst*) cond;
        if (dynamic_cast<EqInst*>(pred_inst) ||
            dynamic_cast<LtSInst*>(pred_inst) ||
            dynamic_cast<LtUInst*>(pred_inst)) {
          build_cmp(pred_inst->arg(0), pred_inst->arg(1));
          if (dynamic_cast<EqInst*>(pred_inst)) {
            _builder.cmove64(res, then);
          } else if (dynamic_cast<LtSInst*>(pred_inst)) {
            _builder.cmovl64(res, then);
          } else if (dynamic_cast<LtUInst*>(pred_inst)) {
            _builder.cmovb64(res, then);
          } else {
            assert(false);
          }
          return;
        }
      }
      
      _builder.test8_imm(vreg(cond), (uint64_t) 1);
      _builder.cmovnz64(res, then);
    }

    void isel(Inst* inst) {
      if (dynmatch(FreezeInst, freeze, inst)) {
        _builder.mov64(vreg(inst), vreg(freeze->arg(0)));
      } else if (dynmatch(SelectInst, select, inst)) {
        _builder.mov64(vreg(inst), vreg(select->arg(2)));
        build_cmov(vreg(inst), select->cond(), vreg(select->arg(1)));
      } else if (dynmatch(ResizeUInst, resize_u, inst)) {
        if (resize_u->arg(0)->type() == Type::Bool) {
          _builder.mov64(vreg(inst), vreg(resize_u->arg(0)));
          _builder.and64_imm(vreg(inst), (uint64_t) 1);
        } else {
          switch (type_size(resize_u->arg(0)->type())) {
            case 1: _builder.movzx8to64(vreg(inst), vreg(resize_u->arg(0))); break;
            case 2: _builder.movzx16to64(vreg(inst), vreg(resize_u->arg(0))); break;
            case 4: _builder.mov32(vreg(inst), vreg(resize_u->arg(0))); break;
            case 8: _builder.mov64(vreg(inst), vreg(resize_u->arg(0))); break;
            default:
              assert(false && "Unsupported resize type");
          }
        }
      } else if (dynmatch(ResizeSInst, resize_s, inst)) {
        if (resize_s->arg(0)->type() == Type::Bool) {
          _builder.mov64_imm(vreg(inst), (uint64_t) 0);
          Reg ones = vreg();
          _builder.mov64_imm(ones, ~(uint64_t) 0);
          build_cmov(vreg(inst), resize_s->arg(0), ones);
        } else {
          switch (type_size(resize_s->arg(0)->type())) {
            case 1: _builder.movsx8to64(vreg(inst), vreg(resize_s->arg(0))); break;
            case 2: _builder.movsx16to64(vreg(inst), vreg(resize_s->arg(0))); break;
            case 4: _builder.movsx32to64(vreg(inst), vreg(resize_s->arg(0))); break;
            case 8: _builder.mov64(vreg(inst), vreg(resize_s->arg(0))); break;
            default:
              assert(false && "Unsupported resize type");
          }
        }
      } else if (dynmatch(ResizeXInst, resize_x, inst)) {
        _builder.mov64(vreg(inst), vreg(resize_x->arg(0)));
      } else if (dynmatch(LoadInst, load, inst)) {
        X86Inst::Mem mem(vreg(load->arg(0)), load->offset());
        switch (type_size(load->type())) {
          case 1: _builder.mov8(vreg(inst), mem); break;
          case 2: _builder.mov16(vreg(inst), mem); break;
          case 4: _builder.mov32(vreg(inst), mem); break;
          case 8: _builder.mov64(vreg(inst), mem); break;
          default:
            assert(false && "Unsupported load type");
        }
      } else if (dynmatch(StoreInst, store, inst)) {
        X86Inst::Mem mem(vreg(store->arg(0)), store->offset());
        if (dynmatch(Const, constant_value, store->arg(1))) {
          switch (type_size(store->arg(1)->type())) {
            case 1: _builder.mov8_imm(mem, constant_value->value()); return;
            case 2: _builder.mov16_imm(mem, constant_value->value()); return;
            case 4: _builder.mov32_imm(mem, constant_value->value()); return;
            case 8:
              if (is_sext_imm32(constant_value)) {
                _builder.mov64_imm(mem, constant_value->value());
                return;
              }
            break;
            default:
              assert(false && "Unsupported store type");
          }
        } else if (dynmatch(AddInst, add, store->arg(1))) {
          LoadInst* load_arg = nullptr;
          Value* other_arg = nullptr;

          #define find_load(load_index, other_index) \
            if (dynmatch(LoadInst, load, add->arg(load_index))) { \
              bool exact_aliasing_matches = load->aliasing() == store->aliasing() && load->aliasing() < 0; \
              bool ptr_offset_matches = load->arg(0) == store->arg(0) && load->offset() == store->offset(); \
              if (_memory_deps.at(load) == _memory_deps.at(store) && (exact_aliasing_matches || ptr_offset_matches)) { \
                load_arg = load; \
                other_arg = add->arg(other_index); \
              } \
            }
          
          find_load(0, 1);
          find_load(1, 0);

          #undef find_load

          if (load_arg) {
            switch (type_size(store->arg(1)->type())) {
              case 1: _builder.add8_mem(mem, vreg(other_arg)); return;
              case 2: _builder.add16_mem(mem, vreg(other_arg)); return;
              case 4: _builder.add32_mem(mem, vreg(other_arg)); return;
              case 8: _builder.add64_mem(mem, vreg(other_arg)); return;
              default:
                assert(false && "Unsupported store type");
            }
          }
        }

        if (store->arg(1)->type() == Type::Bool) {
          _builder.and64_imm(vreg(store->arg(1)), (uint64_t) 1);
        }

        switch (type_size(store->arg(1)->type())) {
          case 1: _builder.mov8_mem(mem, vreg(store->arg(1))); break;
          case 2: _builder.mov16_mem(mem, vreg(store->arg(1))); break;
          case 4: _builder.mov32_mem(mem, vreg(store->arg(1))); break;
          case 8: _builder.mov64_mem(mem, vreg(store->arg(1))); break;
          default:
            assert(false && "Unsupported store type");
        }
      } else if (dynmatch(AddPtrInst, add_ptr, inst)) {
        build_add(vreg(inst), add_ptr->ptr(), add_ptr->offset());
      } else if (dynmatch(AddInst, add, inst)) {
        build_add(vreg(inst), add->arg(0), add->arg(1));
      } else if (dynmatch(SubInst, sub, inst)) {
        _builder.mov64(vreg(inst), vreg(sub->arg(0)));

        if (dynmatch(Const, constant_b, sub->arg(1))) {
          if (is_sext_imm32(constant_b)) {
            _builder.sub64_imm(vreg(inst), constant_b->value());
            return;
          }
        }

        _builder.sub64(vreg(inst), vreg(sub->arg(1)));
      } else if (dynmatch(MulInst, mul, inst)) {
        _builder.mov64(vreg(inst), vreg(mul->arg(0)));
        _builder.imul64(vreg(inst), vreg(mul->arg(1)));
      } else if (dynamic_cast<DivUInst*>(inst) ||
                 dynamic_cast<ModUInst*>(inst) ||
                 dynamic_cast<DivSInst*>(inst) ||
                 dynamic_cast<ModSInst*>(inst)) {

        Reg rdx = fix_to_preg(vreg(), REG_RDX);
        Reg rax = fix_to_preg(vreg(), REG_RAX);

        if (dynamic_cast<DivSInst*>(inst) || dynamic_cast<ModSInst*>(inst)) {
          if (inst->type() == Type::Int8) {
            _builder.movsx8to64(rax, vreg(inst->arg(0)));
            _builder.movsx8to64(vreg(inst->arg(1)), vreg(inst->arg(1)));
          } else {
            _builder.mov64(rax, vreg(inst->arg(0)));
          }

          switch (inst->type()) {
            case Type::Int8:
            case Type::Int16:
              _builder.cwd(rdx);
              _builder.idiv16(vreg(inst->arg(1)));
            break;
            case Type::Int32:
              _builder.cdq(rdx);
              _builder.idiv32(vreg(inst->arg(1)));
            break;
            case Type::Int64:
              _builder.cqo(rdx);
              _builder.idiv64(vreg(inst->arg(1)));
            break;
            default: assert(false && "Unsupported type");
          }
        } else {
          _builder.mov64_imm(rdx, (uint64_t) 0);
          if (inst->type() == Type::Int8) {
            _builder.movzx8to64(rax, vreg(inst->arg(0)));
            _builder.movzx8to64(vreg(inst->arg(1)), vreg(inst->arg(1)));
          } else {
            _builder.mov64(rax, vreg(inst->arg(0)));
          }

          switch (inst->type()) {
            case Type::Int8:
            case Type::Int16: _builder.div16(vreg(inst->arg(1))); break;
            case Type::Int32: _builder.div32(vreg(inst->arg(1))); break;
            case Type::Int64: _builder.div64(vreg(inst->arg(1))); break;
            default: assert(false && "Unsupported type");
          }
        }

        if (dynamic_cast<ModUInst*>(inst) || dynamic_cast<ModSInst*>(inst)) {
          _builder.mov64(vreg(inst), rdx);
          _builder.pseudo_use(rax);
        } else {
          _builder.mov64(vreg(inst), rax);
          _builder.pseudo_use(rdx);
        }
      } else if (dynmatch(AndInst, and_inst, inst)) {
        _builder.mov64(vreg(inst), vreg(and_inst->arg(0)));

        if (dynmatch(Const, constant_b, and_inst->arg(1))) {
          if (is_sext_imm32(constant_b)) {
            _builder.and64_imm(vreg(inst), constant_b->value());
            return;
          }
        }

        _builder.and64(vreg(inst), vreg(and_inst->arg(1)));
      } else if (dynmatch(OrInst, or_inst, inst)) {
        _builder.mov64(vreg(inst), vreg(or_inst->arg(0)));

        if (dynmatch(Const, constant_b, or_inst->arg(1))) {
          if (is_sext_imm32(constant_b)) {
            _builder.or64_imm(vreg(inst), constant_b->value());
            return;
          }
        }

        _builder.or64(vreg(inst), vreg(or_inst->arg(1)));
      } else if (dynmatch(XorInst, xor_inst, inst)) {
        _builder.mov64(vreg(inst), vreg(xor_inst->arg(0)));

        if (dynmatch(Const, constant_b, xor_inst->arg(1))) {
          if (is_sext_imm32(constant_b)) {
            _builder.xor64_imm(vreg(inst), constant_b->value());
            return;
          }
        }

        _builder.xor64(vreg(inst), vreg(xor_inst->arg(1)));
      } else if (dynmatch(ShlInst, shl, inst)) {
        _builder.mov64(vreg(inst), vreg(shl->arg(0)));

        if (dynmatch(Const, constant_b, shl->arg(1))) {
          _builder.shl64_imm(vreg(inst), constant_b->value());
          return;
        }

        _builder.mov64(fix_to_preg(vreg(), REG_RCX), vreg(shl->arg(1)));
        _builder.shl64(vreg(inst));
      } else if (dynmatch(ShrUInst, shr_u, inst)) {
        _builder.mov64(vreg(inst), vreg(shr_u->arg(0)));

        if (dynmatch(Const, constant_b, shr_u->arg(1))) {
          switch (type_size(shr_u->arg(0)->type())) {
            case 1: _builder.shr8_imm(vreg(inst), constant_b->value()); break;
            case 2: _builder.shr16_imm(vreg(inst), constant_b->value()); break;
            case 4: _builder.shr32_imm(vreg(inst), constant_b->value()); break;
            case 8: _builder.shr64_imm(vreg(inst), constant_b->value()); break;
            default: assert(false && "Unsupported type");
          }
          return;
        }

        _builder.mov64(fix_to_preg(vreg(), REG_RCX), vreg(shr_u->arg(1)));
        switch (type_size(shr_u->arg(0)->type())) {
          case 1: _builder.shr8(vreg(inst)); break;
          case 2: _builder.shr16(vreg(inst)); break;
          case 4: _builder.shr32(vreg(inst)); break;
          case 8: _builder.shr64(vreg(inst)); break;
          default: assert(false && "Unsupported type");
        }
      } else if (dynmatch(ShrSInst, shr_s, inst)) {
        _builder.mov64(vreg(inst), vreg(shr_s->arg(0)));

        if (dynmatch(Const, constant_b, shr_s->arg(1))) {
          switch (type_size(shr_s->arg(0)->type())) {
            case 1: _builder.sar8_imm(vreg(inst), constant_b->value()); break;
            case 2: _builder.sar16_imm(vreg(inst), constant_b->value()); break;
            case 4: _builder.sar32_imm(vreg(inst), constant_b->value()); break;
            case 8: _builder.sar64_imm(vreg(inst), constant_b->value()); break;
            default: assert(false && "Unsupported type");
          }
          return;
        }

        _builder.mov64(fix_to_preg(vreg(), REG_RCX), vreg(shr_s->arg(1)));
        switch (type_size(shr_s->arg(0)->type())) {
          case 1: _builder.sar8(vreg(inst)); break;
          case 2: _builder.sar16(vreg(inst)); break;
          case 4: _builder.sar32(vreg(inst)); break;
          case 8: _builder.sar64(vreg(inst)); break;
          default: assert(false && "Unsupported type");
        }
      } else if (dynamic_cast<EqInst*>(inst) ||
                 dynamic_cast<LtSInst*>(inst) ||
                 dynamic_cast<LtUInst*>(inst)) {
        build_cmp(inst->arg(0), inst->arg(1));
        if (dynamic_cast<EqInst*>(inst)) {
          _builder.sete8(vreg(inst));
        } else if (dynamic_cast<LtSInst*>(inst)) {
          _builder.setl8(vreg(inst));
        } else if (dynamic_cast<LtUInst*>(inst)) {
          _builder.setb8(vreg(inst));
        } else {
          assert(false);
        }
      } else if (dynmatch(BranchInst, branch, inst)) {
        if (branch->arg(0)->is_inst()) {
          Inst* pred_inst = (Inst*) branch->arg(0);
          if (dynamic_cast<EqInst*>(pred_inst) ||
              dynamic_cast<LtSInst*>(pred_inst) ||
              dynamic_cast<LtUInst*>(pred_inst)) {
            build_cmp(pred_inst->arg(0), pred_inst->arg(1));
            if (dynamic_cast<EqInst*>(pred_inst)) {
              _builder.je(_blocks[branch->true_block()->name()]);
            } else if (dynamic_cast<LtSInst*>(pred_inst)) {
              _builder.jl(_blocks[branch->true_block()->name()]);
            } else if (dynamic_cast<LtUInst*>(pred_inst)) {
              _builder.jb(_blocks[branch->true_block()->name()]);
            } else {
              assert(false);
            }
            _builder.jmp(_blocks[branch->false_block()->name()]);
            return;
          }
        }

        _builder.test8_imm(vreg(branch->cond()), (uint64_t) 1);
        _builder.jne(_blocks[branch->true_block()->name()]);
        _builder.jmp(_blocks[branch->false_block()->name()]);
      } else if (dynmatch(JumpInst, jump, inst)) {
        Reg copies[jump->block()->args().size()];
        for (Arg* arg : jump->block()->args()) {
          copies[arg->index()] = vreg();
          _builder.mov64(copies[arg->index()], vreg(jump->arg(arg->index())));
        }
        for (Arg* arg : jump->block()->args()) {
          _builder.mov64(vreg(arg), copies[arg->index()]);
        }
        _builder.jmp(_blocks[jump->block()->name()]);
      } else if (dynmatch(ExitInst, exit, inst)) {
        _builder.ret();
      } else {
        inst->write(std::cerr);
        std::cerr << std::endl;
        assert(false && "Unknown instruction");
      }
    }

    void isel() {
      for (Block* block : _section->rev_range()) {
        X86Block* x86block = _blocks[block->name()];
        
        // Keep track of backedges, to identify loops
        if (dynmatch(JumpInst, jump, block->terminator())) {
          _blocks[jump->block()->name()]->add_incoming(x86block);
        } else if (dynmatch(BranchInst, branch, block->terminator())) {
          _blocks[branch->true_block()->name()]->add_incoming(x86block);
          _blocks[branch->false_block()->name()]->add_incoming(x86block);
        }

        // isel
        _builder.set_block(x86block);
        for (Inst* inst : block->rev_range()) {
          _builder.move_before(_builder.block(), _builder.block()->first());
          if (inst->has_side_effect() ||
              inst->is_terminator() ||
              !_vregs.at(inst).is_invalid()) {
            isel(inst);
          }
        }

        // Add pseudo uses after loops
        if (x86block->loop()) {
          // We added an extra block at the end, so this should always be the case
          assert(x86block->loop()->name() + 1 < _blocks.size());
          X86Block* after_end = _blocks[x86block->loop()->name() + 1];

          // The only instructions that can be live across loops are those
          // which are defined before the loop (name < (*loop->begin())->name())
          // and used inside the loop (vreg exists)

          size_t max_name = (*block->begin())->name();
          assert(max_name < _section->name_count());
          _builder.move_to_begin(after_end);
          for (size_t it = 0; it < max_name; it++) {
            Reg vreg = _vregs.at_name(it);
            if (vreg.is_virtual()) {
              _builder.pseudo_use(vreg);
            }
          }
        }
      }
    }

    void autoname_insts() {
      size_t inst_name = 0;
      for (X86Block* block : _blocks) {
        for (X86Inst* inst : *block) {
          inst->set_name(inst_name++);
        }
      }
    }

    class StackOffsetAlloc {
    private:
      size_t _max_offset = 0;
      std::vector<size_t> _returned_offsets;
    public:
      StackOffsetAlloc() {}

      size_t alloc() {
        if (_returned_offsets.empty()) {
          _max_offset += 8;
          return _max_offset;
        } else {
          size_t offset = _returned_offsets.back();
          _returned_offsets.pop_back();
          return offset;
        }
      }

      void free(size_t offset) {
        _returned_offsets.push_back(offset);
      }
    };

    constexpr static Reg REG_RAX = Reg::phys(0);
    constexpr static Reg REG_RCX = Reg::phys(1);
    constexpr static Reg REG_RDX = Reg::phys(2);
    constexpr static Reg REG_RBX = Reg::phys(3);
    constexpr static Reg REG_RSP = Reg::phys(4);
    constexpr static Reg REG_RBP = Reg::phys(5);

    class RegFileState {
    private:
      std::vector<Reg> _regs;
      uint16_t _free = 0xffff;
      uint16_t _max_free = 0xffff;
      
      std::vector<size_t> _lru;
      size_t _lru_count = 0;
    public:
      RegFileState() {
        _regs.resize(16, Reg());
        _lru.resize(16, 0);

        disable(REG_RSP);
        disable(REG_RBP);

        _free = _max_free;
      }

      size_t size() const {
        return _regs.size();
      }

      void disable(Reg preg) {
        assert(preg.is_physical());
        _max_free &= ~(1 << preg.id());
        _lru[preg.id()] = ~size_t(0);
      }

      const Reg operator[](Reg reg) const {
        assert(reg.is_physical());
        return _regs[reg.id()];
      }

      void set(Reg preg, Reg vreg) {
        assert(preg.is_physical() && vreg.is_virtual());
        _regs[preg.id()] = vreg;
        _free &= ~(1 << preg.id());
      }

      void touch(Reg preg) {
        assert(preg.is_physical());
        _lru[preg.id()] = _lru_count++;
      }

      void free(Reg preg) {
        assert(preg.is_physical());
        _regs[preg.id()] = Reg();
        _free |= (1 << preg.id());
      }

      bool is_free(Reg preg) const {
        assert(preg.is_physical());
        return (_free & (1 << preg.id())) != 0;
      }

      bool is_disabled(Reg preg) const {
        assert(preg.is_physical());
        return (_max_free & (1 << preg.id())) == 0;
      }

      Reg get_free_reg() {
        if (_free == 0) {
          return Reg();
        } else {
          return Reg::phys(__builtin_ctz(_free));
        }
      }

      Reg get_lru() {
        size_t min_index = 0;
        size_t min_value = ~size_t(0);
        for (size_t it = 0; it < _lru.size(); it++) {
          if (_lru[it] < min_value) {
            min_value = _lru[it];
            min_index = it;
          }
        }
        return Reg::phys(min_index);
      }

      #ifndef NDEBUG
      void assert_invariant() const {
        for (size_t it = 0; it < _regs.size(); it++) {
          if (!is_disabled(Reg::phys(it))) {
            assert(_regs[it].is_invalid() == is_free(Reg::phys(it)));
          }
        }
      }
      #else
      __attribute__((always_inline))
      void assert_invariant() const {}
      #endif

      void load_state(Reg* state) {
        _free = _max_free;
        for (size_t it = 0; it < _regs.size(); it++) {
          _regs[it] = state[it];
          if (!_regs[it].is_invalid()) {
            _free &= ~(1 << it);
          }
        }
        assert_invariant();
      }

      void merge_state(Reg* state) {
        assert_invariant();
        for (size_t it = 0; it < _regs.size(); it++) {
          if (state[it] != _regs[it]) {
            state[it] = Reg();
          }
        }
      }

      void write_state(std::ostream& stream, const Reg* regs) const {
        stream << "[";
        for (size_t it = 0; it < _regs.size(); it++) {
          if (it != 0) {
            stream << ", ";
          }
          if (regs[it].is_invalid()) {
            stream << "<free>";
          } else if (is_disabled(Reg::phys(it))) {
            stream << "<disabled>";
          } else {
            stream << regs[it];
          }
        }
        stream << "]";
      }

      void write(std::ostream& stream) const {
        assert_invariant();
        write_state(stream, _regs.data());
      }
    };

    StackOffsetAlloc _stack_offset_alloc;

    void spill(RegFileState& reg_file, Reg preg, bool allow_spill_to_reg = true) {
      Reg vreg = reg_file[preg];
      if (vreg.is_virtual()) {
        VRegInfo& info = _vreg_info[vreg.id()];
        Reg free_reg = reg_file.get_free_reg();
        if (allow_spill_to_reg && free_reg.is_physical()) {
          // No need to spill, just move to free reg
          _builder.mov64(free_reg, preg);
          reg_file.free(preg);
          info.current_reg = free_reg;
          reg_file.set(free_reg, vreg);
        } else {
          if (info.stack_offset == 0) {
            info.stack_offset = _stack_offset_alloc.alloc();
            assert(info.stack_offset != 0);
          }
          _builder.mov64_mem(
            X86Inst::Mem(
              REG_RSP,
              -(int64_t) info.stack_offset
            ),
            preg
          );
          reg_file.free(preg);
          info.current_reg = Reg();
        }
      }
    }

    void unspill(RegFileState& reg_file, Reg vreg, Reg preg) {
      assert(vreg.is_virtual());
      VRegInfo& info = _vreg_info[vreg.id()];
      if (info.current_reg.is_physical()) {
        // Ne need to unspill, just move from current reg
        _builder.mov64(preg, info.current_reg);
        reg_file.free(info.current_reg);
      } else {
        assert(info.stack_offset != 0);
        _builder.mov64(
          preg,
          X86Inst::Mem(
            REG_RSP,
            -(int64_t) info.stack_offset
          )
        );
      }
      info.current_reg = preg;
      reg_file.set(preg, vreg);
    }

    void spill_and_unspill(RegFileState& reg_file,
                           Reg preg,
                           Reg vreg,
                           bool is_def,
                           bool allow_spill_to_reg = true) {
      assert(preg.is_physical());
      spill(reg_file, preg, allow_spill_to_reg);
      if (vreg.is_virtual()) {
        VRegInfo& info = _vreg_info[vreg.id()];
        if (is_def) {
          info.current_reg = preg;
          reg_file.set(preg, vreg);
        } else {
          unspill(reg_file, vreg, preg);
        }
        reg_file.touch(preg); 
      }
    }

    bool is_foldable_mov(X86Inst* inst) {
      if (inst->kind() == X86Inst::Kind::Mov64 &&
          std::holds_alternative<Reg>(inst->rm())) {
        Reg src = std::get<Reg>(inst->rm());
        Reg dst = inst->reg();
        assert(src.is_virtual() && dst.is_virtual());
        if (_vreg_info[src.id()].current_reg.is_physical() &&
            _vreg_info[src.id()].interval.max == inst->name() &&
            _vreg_info[dst.id()].interval.min == inst->name() &&
            _vreg_info[dst.id()].fixed.is_invalid()) {
          return true;
        }
      } 
      return false;
    }

    void load_state(RegFileState& reg_file, Reg* state) {
      for (size_t it = 0; it < reg_file.size(); it++) {
        Reg preg = Reg::phys(it);
        if (reg_file[preg].is_virtual()) {
          _vreg_info[reg_file[preg].id()].current_reg = Reg();
        }
      }
      reg_file.load_state(state);
      for (size_t it = 0; it < reg_file.size(); it++) {
        Reg preg = Reg::phys(it);
        if (reg_file[preg].is_virtual()) {
          _vreg_info[reg_file[preg].id()].current_reg = preg;
        }
      }
    }

    void spill_all(RegFileState& reg_file) {
      for (size_t it = 0; it < reg_file.size(); it++) {
        if (!reg_file.is_free(Reg::phys(it))) {
          spill(reg_file, Reg::phys(it), false);
        }
      }
    }

    bool is_def_only(Reg reg, X86Inst* inst) {
      VRegInfo& info = _vreg_info[reg.id()];

      // Special case for mov instructions.
      // Since they are used for block arguments, the register may
      // be def-only even if the instruction is not the first in
      // the register's live interval.
      if (inst->kind() == X86Inst::Kind::Mov64 &&
          std::holds_alternative<Reg>(inst->rm())) {
        Reg src = std::get<Reg>(inst->rm());
        Reg dst = inst->reg();
        if (reg != src && reg == dst) {
          return true;
        }
      }

      return inst->name() == info.interval.min; 
    }

    void regalloc() {
      for (X86Block* block : _blocks) {
        for (X86Inst* inst : *block) {
          inst->visit_regs([&](Reg reg) {
            assert(reg.is_virtual());
            _vreg_info[reg.id()].interval.incl(inst->name());
          });
        }
      }

      RegFileState reg_file;

      Reg* initial_state = (Reg*) _allocator.alloc(sizeof(Reg) * reg_file.size(), alignof(Reg));
      std::fill(initial_state, initial_state + reg_file.size(), Reg());

      for (Arg* arg : _section->entry()->args()) {
        VRegInfo& info = _vreg_info[vreg(arg).id()];
        assert(info.fixed.is_physical() && "Entry arguments must be in fixed registers");
        initial_state[info.fixed.id()] = vreg(arg);
      }
      _blocks[0]->set_regalloc(initial_state);

      for (X86Block* block : _blocks) {
        if (block->regalloc()) {
          load_state(reg_file, block->regalloc());
        }

        for (auto it = block->begin(); it != block->end(); ) {
          X86Inst* inst = *it;
          _builder.move_before(block, inst);

          if (inst->kind() == X86Inst::Kind::PseudoUse) {
            it = it.erase(); // Not needed after regalloc
          } else if (is_foldable_mov(inst)) {
            Reg src = std::get<Reg>(inst->rm());
            Reg dst = inst->reg();
            VRegInfo& src_info = _vreg_info[src.id()];
            VRegInfo& dst_info = _vreg_info[dst.id()];

            dst_info.current_reg = src_info.current_reg;
            reg_file.set(src_info.current_reg, dst);
            reg_file.touch(src_info.current_reg);
            src_info.current_reg = Reg();

            it = it.erase();
            continue;
          } else {
            inst->visit_regs([&](Reg reg) {
              VRegInfo& info = _vreg_info[reg.id()];
              if (info.current_reg.is_invalid() && info.fixed.is_physical()) {
                spill_and_unspill(reg_file, info.fixed, reg, is_def_only(reg, inst));
              }
            });

            inst->visit_regs([&](Reg reg) {
              VRegInfo& info = _vreg_info[reg.id()];
              if (info.current_reg.is_invalid() && !info.fixed.is_physical()) {
                Reg preg = reg_file.get_free_reg();
                if (!preg.is_physical()) {
                  preg = reg_file.get_lru();
                }
                spill_and_unspill(reg_file, preg, reg, is_def_only(reg, inst));
              }
            });

            inst->visit_regs([&](Reg& reg) {
              VRegInfo& info = _vreg_info[reg.id()];
              assert(info.current_reg.is_physical());
              reg_file.touch(info.current_reg);
              reg = info.current_reg;
            });

            it++;
          }

          inst->visit_regs([&](Reg& reg) {
            VRegInfo* info;
            if (reg.is_virtual()) {
              info = &_vreg_info[reg.id()];
              if (info->current_reg.is_invalid()) {
                return; // Already deallocated
              }
            } else {
              if (reg_file[reg].is_invalid()) {
                return; // Already deallocated
              }
              info = &_vreg_info[reg_file[reg].id()];
            }
            if (inst->name() == info->interval.max) {
              reg_file.free(info->current_reg);
              info->current_reg = Reg();
              if (info->stack_offset != 0) {
                _stack_offset_alloc.free(info->stack_offset);
              }
            }
          });

          if (std::holds_alternative<X86Block*>(inst->imm())) {
            X86Block* target = std::get<X86Block*>(inst->imm());
            if (target->regalloc()) {
              // Restore regalloc state
              assert(inst->kind() == X86Inst::Kind::Jmp); // Merges may only be unconditional jumps
              if (target->name() < block->name()) {
                // Backedge
                assert(target->loop());
              }
              for (size_t it = 0; it < reg_file.size(); it++) {
                Reg preg = Reg::phys(it);
                Reg current_vreg = reg_file[preg];
                Reg target_vreg = target->regalloc()[it];
                if (current_vreg != target_vreg) {
                  spill_and_unspill(reg_file, preg, target_vreg, /*is_def=*/ false, /*allow_spill_to_reg=*/ false);
                }
              }
              #ifndef NDEBUG
              for (size_t it = 0; it < reg_file.size(); it++) {
                assert(reg_file[Reg::phys(it)] == target->regalloc()[it]);
              }
              #endif
            } else {
              Reg* state = (Reg*) _allocator.alloc(sizeof(Reg) * reg_file.size(), alignof(Reg));
              for (size_t it = 0; it < reg_file.size(); it++) {
                Reg reg = reg_file[Reg::phys(it)];
                if (reg.is_virtual() && _vreg_info[reg.id()].interval.max >= target->first()->name()) {
                  state[it] = reg;
                } else {
                  state[it] = Reg();
                }
              }
              target->set_regalloc(state);
            }
          }
        }
      }
    }

    void peephole() {
      for (X86Block* block : _blocks) {
        for (auto it = block->begin(); it != block->end(); ) {
          X86Inst* inst = *it;

          if (((inst->kind() == X86Inst::Kind::Mov8Imm ||
                inst->kind() == X86Inst::Kind::Mov32Imm ||
                inst->kind() == X86Inst::Kind::Mov64Imm) &&
              std::holds_alternative<Reg>(inst->rm())) ||
              inst->kind() == X86Inst::Kind::Mov64Imm64) {
            if (std::holds_alternative<uint64_t>(inst->imm())) {
              uint64_t value = std::get<uint64_t>(inst->imm());
              if (value == 0) {
                // Replace with xor reg, reg
                inst->set_kind(X86Inst::Kind::Xor64);
                inst->set_imm(std::monostate());
                if (inst->kind() == X86Inst::Kind::Mov64Imm64) {
                  inst->set_rm(inst->reg());
                } else {
                  inst->set_reg(std::get<Reg>(inst->rm()));
                }
              }
            }
          } else if (inst->kind() == X86Inst::Kind::Jmp &&
                      inst->next() == nullptr &&
                      std::holds_alternative<X86Block*>(inst->imm()) &&
                      block->name() + 1 < _blocks.size()) {
            X86Block* target_block = std::get<X86Block*>(inst->imm());
            if (target_block == _blocks[block->name() + 1]) {
              it = it.erase();
              continue;
            }
          }

          ++it;
        }
      }
    }

    void run(const std::vector<Reg>& input_pregs) {
      _memory_deps.init(_section);
      _vregs.init(_section);

      for (Arg* arg : _section->entry()->args()) {
        fix_to_preg(vreg(arg), input_pregs[arg->index()]);
      }

      // We create one extra block for pseudo_use instructions after loops
      _blocks.resize(_section->block_count() + 1, nullptr);
      for (size_t it = 0; it < _section->block_count() + 1; it++) {
        X86Block* x86_block = _builder.build_block();
        x86_block->set_name(it);
        _blocks[it] = x86_block;
      }

      memory_deps();
      isel();
      autoname_insts();
      regalloc();
      peephole();
    }
  public:
    X86CodeGen(Section* section, const std::vector<Reg>& input_pregs):
        Pass(section),
        _section(section),
        _allocator(section->allocator()),
        _builder(section->allocator(), nullptr) {

      run(input_pregs);
    }

    X86CodeGen(Section* section,
               Allocator& allocator,
               const std::vector<Reg>& input_pregs):
        Pass(section),
        _section(section),
        _allocator(allocator),
        _builder(allocator, nullptr) {
      
      run(input_pregs);
    }

    struct Label {
      size_t pos = 0;
      size_t size = 0;
      size_t ref = 0;
      X86Block* to = nullptr;

      Label() {}
    };

    void emit(std::vector<uint8_t>& buffer) {
      std::vector<Label> labels;
      std::vector<size_t> offsets(_blocks.size(), 0);
      for (X86Block* block : _blocks) {
        offsets[block->name()] = buffer.size();
        for (X86Inst* inst : *block) {
          emit(inst, buffer, labels);
        }
      }

      for (const Label& label : labels) {
        int64_t value = offsets[label.to->name()] - label.ref;
        for (size_t it = 0; it < label.size; it++) {
          buffer[label.pos + it] = (value >> (it * 8)) & 0xff;
        }
      }
    }

    void emit(X86Inst* inst, std::vector<uint8_t>& buffer, std::vector<Label>& labels) {
      Reg reg = inst->reg();
      X86Inst::RM rm = inst->rm();
      std::optional<uint64_t> imm;
      if (std::holds_alternative<uint64_t>(inst->imm())) {
        imm = std::get<uint64_t>(inst->imm());
      } else if (std::holds_alternative<X86Block*>(inst->imm())) {
        imm = 0; // Inserted later
      }

      auto byte = [&](uint8_t value) {
        buffer.push_back(value);
      };

      auto rex = [&](bool w = false) {
        uint8_t rex = 0x40;
        if (w) {
          rex |= 0x08;
        }
        rex |= ((reg.id() >> 3) & 1) << 2; // R
        if (std::holds_alternative<Reg>(rm)) {
          rex |= ((std::get<Reg>(rm).id() >> 3) & 1); // B
        } else if (std::holds_alternative<X86Inst::Mem>(rm)) {
          X86Inst::Mem mem = std::get<X86Inst::Mem>(rm);
          rex |= ((mem.base.id() >> 3) & 1); // B
          if (!mem.index.is_invalid()) {
            rex |= ((mem.index.id() >> 3) & 1) << 1; // X
          }
        }
        byte(rex);
      };

      auto rex_w = [&]() {
        rex(true);
      };

      auto rex_opt = [&]() {
        bool need_rex = false;
        if (reg.id() >= 8) {
          need_rex = true;
        } else if (std::holds_alternative<Reg>(rm)) {
          if (std::get<Reg>(rm).id() >= 8) {
            need_rex = true;
          }
        } else if (std::holds_alternative<X86Inst::Mem>(rm)) {
          X86Inst::Mem mem = std::get<X86Inst::Mem>(rm);
          if (mem.base.id() >= 8) {
            need_rex = true;
          }
          if (!mem.index.is_invalid() && mem.index.id() >= 8) {
            need_rex = true;
          }
        }

        if (need_rex) {
          rex();
        }
      };

      auto modrm = [&]() {
        // Encode ModRM byte
        uint8_t modrm = 0;
        modrm |= (reg.id() & 0b111) << 3; // reg
        if (std::holds_alternative<Reg>(rm)) {
          modrm |= 0b11 << 6; // mod
          modrm |= std::get<Reg>(rm).id() & 0b111; // r/m
          byte(modrm);
        } else if (std::holds_alternative<X86Inst::Mem>(rm)) {
          X86Inst::Mem mem = std::get<X86Inst::Mem>(rm);

          if (mem.disp == 0 && (mem.base.id() & 0b111) != 0b101) {
            modrm |= 0b00 << 6;
          } else if (mem.disp >= -128 && mem.disp <= 127) {
            modrm |= 0b01 << 6;
          } else {
            modrm |= 0b10 << 6;
          }

          if (mem.scale == 0 && (mem.base.id() & 0b111) != 0b100) {
            modrm |= mem.base.id() & 0b111;
            byte(modrm);
          } else {
            modrm |= 0b100; // SIB
            byte(modrm);

            uint8_t scale = 0;
            uint8_t index = 0b100; // none
            uint8_t base = mem.base.id() & 0b111; // none

            switch (mem.scale) {
              case 0: break;
              case 1: scale = 0b00; break;
              case 2: scale = 0b01; break;
              case 4: scale = 0b10; break;
              case 8: scale = 0b11; break;
              default: assert(false && "Invalid scale");
            }

            if (mem.scale != 0 && !mem.index.is_invalid()) {
              index = mem.index.id() & 0b111;
            }

            uint8_t sib = scale << 6 | index << 3 | base;
            byte(sib);
          }

          if (mem.disp != 0 || (mem.base.id() & 0b111) == 0b101) {
            if (mem.disp >= -128 && mem.disp <= 127) {
              byte(mem.disp & 0xff);
            } else {
              byte(mem.disp & 0xff);
              byte((mem.disp >> 8) & 0xff);
              byte((mem.disp >> 16) & 0xff);
              byte((mem.disp >> 24) & 0xff);
            }
          }
        } else {
          inst->write(std::cerr);
          std::cerr << std::endl;

          assert(false && "Incomplete ModRM");
        }
      };

      auto imm_n = [&](size_t size) {
        if (std::holds_alternative<X86Block*>(inst->imm())) {
          Label label;
          label.pos = buffer.size();
          label.size = size;
          label.ref = buffer.size() + size;
          label.to = std::get<X86Block*>(inst->imm());
          labels.push_back(label);
        }
        assert(imm.has_value());
        for (size_t it = 0; it < size; it++) {
          byte((imm.value() >> (it * 8)) & 0xff);
        }
      };

      switch (inst->kind()) {
        #define x86_inst(name, lowercase, usedef, is_64_bit, opcode, ...) \
          case X86Inst::Kind::name: opcode; break;
        #include "x86insts.inc.hpp"

        default: assert(false && "Unknown x86 instruction");
      }
    }

    void save(const std::string& filename) {
      std::vector<uint8_t> buffer;
      emit(buffer);

      std::ofstream file(filename, std::ios::binary);
      if (!file) {
        assert(false && "Failed to open file for writing");
      }
      file.write((const char*) buffer.data(), buffer.size());
    }

    void write(std::ostream& stream) {
      for (X86Block* block : _blocks) {
        block->write(stream);
      }
    }

    void* deploy() {
      std::vector<uint8_t> bytes;
      emit(bytes);

      void* buffer = mmap(
        nullptr,
        bytes.size(),
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0
      );
      assert(buffer != nullptr);

      memcpy(buffer, bytes.data(), bytes.size());

      if (mprotect(buffer, bytes.size(), PROT_READ | PROT_EXEC) == -1) {
        std::cerr << "Failed to set memory protection: ";
        std::cerr << strerrorname_np(errno) << " " << strerror(errno) << std::endl;
        exit(1);
      }

      return buffer;
    }

    size_t inst_count() const {
      size_t count = 0;
      for (X86Block* block : _blocks) {
        for (X86Inst* inst : *block) {
          count++;
        }
      }
      return count;
    }
  };
}
