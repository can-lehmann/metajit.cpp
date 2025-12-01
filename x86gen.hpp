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
    Reg() {}
    Reg(Kind kind, size_t id): _kind(kind), _id(id) {}

    static Reg phys(size_t id) {
      return Reg(Kind::Physical, id);
    }

    static Reg virt(size_t id) {
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

  class X86Inst: public LinkedListItem<X86Inst> {
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
    size_t _misc_count = 0;
    Reg* _misc = nullptr;
    size_t _name = 0;
  public:
    X86Inst(Kind kind): _kind(kind) {}
    
    Kind kind() const { return _kind; }
    Reg reg() const { return _reg; }
    RM rm() const { return _rm; }
    Imm imm() const { return _imm; }
    size_t misc_count() const { return _misc_count; }
    Reg* misc() const { return _misc; }

    X86Inst& set_kind(Kind kind) { _kind = kind; return *this; }
    X86Inst& set_reg(Reg reg) { _reg = reg; return *this; }
    X86Inst& set_rm(const RM& rm) { _rm = rm; return *this; }
    X86Inst& set_imm(const Imm& imm) { _imm = imm; return *this; }
    X86Inst& set_misc(size_t count, Reg* regs) {
      _misc_count = count;
      _misc = regs;
      return *this;
    }

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

      if (_misc) {
        for (size_t it = 0; it < _misc_count; it++) {
          fn(_misc[it]);
        }
      }
    }

    template <class UseFn, class DefFn>
    void visit_usedef(const UseFn& use_fn, const DefFn& def_fn) const {
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
    LinkedList<X86Inst> _insts;
    X86Block* _backedges = nullptr; // If this block is a loop header, linked list of backedges
    X86Block* _next_backedge = nullptr; // Next backedge in linked list
    size_t _name = 0;
  public:
    X86Block() {}

    size_t name() const { return _name; }
    void set_name(size_t name) { _name = name; }

    LinkedList<X86Inst>& insts() { return _insts; }

    auto begin() { return _insts.begin(); }
    auto end() { return _insts.end(); }

    X86Inst* first() { return _insts.first(); }
    X86Inst* last() { return _insts.last(); }

    auto rev_range() { return _insts.rev_range(); }

    X86Block* backedges() const { return _backedges; }
    X86Block* next_backedge() const { return _next_backedge; }

    void add_backedge(X86Block* backedge) {
      backedge->_next_backedge = _backedges;
      _backedges = backedge;
    }

    void insert_before(X86Inst* before, X86Inst* inst) {
      _insts.insert_before(before, inst);
    }

    void write(std::ostream& stream) {
      stream << "b" << _name;
      if (_backedges) {
        stream << " loop(";
        bool is_first = true;
        for (X86Block* backedge = _backedges; backedge; backedge = backedge->_next_backedge) {
          if (!is_first) {
            stream << ", ";
          }
          stream << "b" << backedge->name();
          is_first = false;
        }
        stream << ")";
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

    if (_misc) {
      stream << " misc=[";
      for (size_t it = 0; it < _misc_count; it++) {
        if (it != 0) {
          stream << ", ";
        }
        stream << _misc[it];
      }
      stream << "]";
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

    void move_before(X86Inst* inst) {
      _insert_pos = inst;
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

    #include "x86insts.inc.hpp"

    X86Inst* mov64_imm64(Reg dst, X86Inst::Imm imm) {
      return &build(X86Inst::Kind::Mov64Imm).set_reg(dst).set_imm(imm);
    }
    
    X86Inst* lea64(Reg dst, X86Inst::Mem src) {
      return &build(X86Inst::Kind::Lea64).set_reg(dst).set_rm(src);
    }

    X86Inst* ret() {
      return &build(X86Inst::Kind::Ret);
    }

    X86Inst* div64(Reg a, Reg b, X86Inst::RM rm) {
      Reg* misc = (Reg*) _allocator.alloc(sizeof(Reg) * 2, alignof(Reg));
      misc[0] = a;
      misc[1] = b;
      return &build(X86Inst::Kind::Div64).set_rm(rm).set_misc(2, misc);
    }
  };

  class X86CodeGen: public Pass<X86CodeGen> {
  private:
    Section* _section;
    std::vector<X86Block*> _blocks;
    X86InstBuilder _builder;

    InstMap<void*> _memory_deps;

    InstMap<std::vector<X86Inst>> _isel;

    InstMap<Reg> _vregs;
    std::vector<Reg> _input_vregs;
    std::vector<Reg> _input_pregs;
    size_t _next_vreg = 0;

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
      return Reg::virt(_next_vreg++);
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
      } else if (dynmatch(Input, input, value)) {
        return _input_vregs[input->index()];
      } else if (dynmatch(Inst, inst, value)) {
        if (_vregs.at(inst).is_invalid()) {
          _vregs[inst] = vreg();
        }
        return _vregs.at(inst);
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

    void isel(Inst* inst) {
      if (dynmatch(FreezeInst, freeze, inst)) {
        _builder.mov64(vreg(inst), vreg(freeze->arg(0)));
      } else if (dynmatch(SelectInst, select, inst)) {
        _builder.mov64(vreg(inst), vreg(select->arg(2)));

        Reg a = vreg(select->arg(1)); // Ensure that cmp/test appear directly before cmov
        if (dynmatch(Inst, pred_inst, select->arg(0))) {
          if (dynamic_cast<EqInst*>(pred_inst) ||
              dynamic_cast<LtSInst*>(pred_inst) ||
              dynamic_cast<LtUInst*>(pred_inst)) {
            build_cmp(pred_inst->arg(0), pred_inst->arg(1));
            if (dynamic_cast<EqInst*>(pred_inst)) {
              _builder.cmove64(vreg(inst), a);
            } else if (dynamic_cast<LtSInst*>(pred_inst)) {
              _builder.cmovl64(vreg(inst), a);
            } else if (dynamic_cast<LtUInst*>(pred_inst)) {
              _builder.cmovb64(vreg(inst), a);
            } else {
              assert(false);
            }
            return;
          }
        }
        
        _builder.test8_imm(vreg(select->arg(0)), (uint64_t) 1);
        _builder.cmovnz64(vreg(inst), a);
      } else if (dynmatch(ResizeUInst, resize_u, inst)) {
        _builder.mov64(vreg(inst), vreg(resize_u->arg(0)));
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
      } else if (dynmatch(ModUInst, mod_u, inst)) {
        vreg(inst->arg(0));
        Reg rax = vreg();
        Reg rdx = vreg(inst);
        _builder.mov64(rax, vreg(inst->arg(1)));
        _builder.mov64_imm(rdx, (uint64_t) 0);
        _builder.div64(rdx, rax, vreg(inst->arg(1)));
        vreg(inst);
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

        _builder.shl64(vreg(inst), vreg(shl->arg(1)));
      } else if (dynmatch(ShrUInst, shr_u, inst)) {
        _builder.mov64(vreg(inst), vreg(shr_u->arg(0)));

        if (dynmatch(Const, constant_b, shr_u->arg(1))) {
          _builder.shr64_imm(vreg(inst), constant_b->value());
          return;
        }

        _builder.shr64(vreg(inst), vreg(shr_u->arg(1)));
      } else if (dynmatch(ShrSInst, shr_s, inst)) {
        _builder.mov64(vreg(inst), vreg(shr_s->arg(0)));

        if (dynmatch(Const, constant_b, shr_s->arg(1))) {
          _builder.sar64_imm(vreg(inst), constant_b->value());
          return;
        }

        _builder.sar64(vreg(inst), vreg(shr_s->arg(1)));
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
        if (dynmatch(Inst, pred_inst, branch->arg(0))) {
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
      for (size_t block_id = _section->size(); block_id-- > 0; ) {
        Block* block = (*_section)[block_id];
        _builder.set_block(_blocks[block->name()]);
        for (Inst* inst : block->rev_range()) {
          _builder.move_before(_builder.block()->first());
          if (inst->has_side_effect() ||
              inst->is_terminator() ||
              !_vregs.at(inst).is_invalid()) {
            isel(inst);
          }
        }
      }
    }

    class PhysicalRegSet {
    private:
      uint16_t _regs = 0;
    public:
      PhysicalRegSet() {
        _regs = ~0;
        erase(4); // ESP
        erase(5); // EBP
      }

      void insert(size_t reg_id) {
        _regs |= (1 << reg_id);
      }

      void erase(size_t reg_id) {
        _regs &= ~(1 << reg_id);
      }

      Reg get_any() {
        if (_regs == 0) {
          return Reg();
        }
        return Reg::phys(__builtin_ctz(_regs));
      }
    };

    bool is_reg_mov(X86Inst* inst) {
      if (inst->kind() == X86Inst::Kind::Mov8 ||
          inst->kind() == X86Inst::Kind::Mov16 ||
          inst->kind() == X86Inst::Kind::Mov32 ||
          inst->kind() == X86Inst::Kind::Mov64) {
        return std::holds_alternative<Reg>(inst->rm());
      }
      return false;
    }

    struct Interval {
      size_t min = 0;
      size_t max = 0;

      Interval(): min(~size_t(0)), max(0) {}

      void incl(size_t value) {
        if (value < min) {
          min = value;
        }
        if (value > max) {
          max = value;
        }
      }

      void write(std::ostream& stream) const {
        stream << "[" << min << "; " << max << "]";
      }
    };

    void autoname() {
      size_t block_name = 0;
      for (X86Block* block : _blocks) {
        block->set_name(block_name++);
      }

      size_t inst_name = 0;
      for (X86Block* block : _blocks) {
        for (X86Inst* inst : *block) {
          inst->set_name(inst_name++);
          if (std::holds_alternative<X86Block*>(inst->imm())) {
            X86Block* target = std::get<X86Block*>(inst->imm());
            if (target->name() <= block->name()) {
              target->add_backedge(block);
            }
          }
        }
      }
    }

    void regalloc() {
      std::vector<Interval> intervals(_next_vreg, Interval());
      
      std::set<size_t> live;
      for (size_t block_it = _blocks.size(); block_it-- > 0; ) {
        X86Block* block = _blocks[block_it];
        for (X86Inst* inst : block->rev_range()) {
          inst->visit_usedef(
            [&](Reg reg) {},
            [&](Reg reg) {
              assert(reg.is_virtual());
              live.erase(reg.id());
              intervals[reg.id()].incl(inst->name());
            }
          );

          inst->visit_usedef(
            [&](Reg reg) {
              assert(reg.is_virtual());
              live.insert(reg.id());
              intervals[reg.id()].incl(inst->name());
            },
            [&](Reg reg) {}
          );
        }

        for (X86Block* backedge = block->backedges(); backedge; backedge = backedge->next_backedge()) {
          for (size_t id : live) {
            intervals[id].incl(backedge->last()->name() + 1);
          }
        }
      }

      std::vector<size_t> kill_order;
      kill_order.reserve(intervals.size());
      for (size_t id = 0; id < intervals.size(); id++) {
        kill_order.push_back(id);
      }

      std::sort(
        kill_order.begin(),
        kill_order.end(),
        [&](size_t a, size_t b) {
          return intervals[a].max < intervals[b].max;
        }
      );

      std::vector<Reg> mapping(_next_vreg, Reg());
      PhysicalRegSet available_regs;
      size_t kill_index = 0;

      for (Input* input : _section->inputs()) {
        Reg phys_reg = _input_pregs[input->index()];
        mapping[_input_vregs[input->index()].id()] = phys_reg;
        available_regs.erase(phys_reg.id());
      }

      for (X86Block* block : _blocks) {
        for (X86Inst* inst : *block) {
          while (kill_index < kill_order.size() &&
                 intervals[kill_order[kill_index]].max <= inst->name()) {
            size_t reg_id = kill_order[kill_index];
            if (!mapping[reg_id].is_invalid()) {
              available_regs.insert(mapping[reg_id].id());
            }
            kill_index++;
          }

          if (is_reg_mov(inst)) {
            Reg src_reg = std::get<Reg>(inst->rm());
            Reg dst_reg = inst->reg();
            assert(src_reg.is_virtual() && dst_reg.is_virtual());
            if (intervals[src_reg.id()].max == intervals[dst_reg.id()].min) {
              Reg phys_reg = mapping[src_reg.id()];
              mapping[dst_reg.id()] = phys_reg;
              available_regs.erase(phys_reg.id());
            }
          }

          inst->visit_regs([&](Reg& reg) {
            assert(reg.is_virtual());

            if (mapping[reg.id()].is_invalid()) {
              Reg phys_reg = available_regs.get_any();
              assert(!phys_reg.is_invalid() && "Ran out of physical registers");
              mapping[reg.id()] = phys_reg;
              available_regs.erase(phys_reg.id());
            }

            reg = mapping[reg.id()];
          });
        }
      }
    }

    bool is_noop(X86Inst* inst) {
      if (is_reg_mov(inst)) {
        if (std::holds_alternative<Reg>(inst->rm())) {
          Reg src_reg = std::get<Reg>(inst->rm());
          return src_reg == inst->reg();
        }
      }
      return false;
    }

    class RegPermutation {
    private:
      X86InstBuilder& _builder;
      uint8_t _from[16];
      uint8_t _to[16];
      std::vector<std::pair<uint8_t, uint8_t>> _swaps;
    public:
      RegPermutation(X86InstBuilder& builder): _builder(builder) {
        for (size_t it = 0; it < 16; it++) {
          _from[it] = it;
          _to[it] = it;
        }
      }

      void xchg(uint8_t a, uint8_t b) {
        if (a == b) {
          return;
        }

        _builder.xchg64(Reg::phys(a), Reg::phys(b));
        _to[_from[a]] = b;
        _to[_from[b]] = a;
        std::swap(_from[a], _from[b]);
        _swaps.push_back({a, b});
      }

      void xchg(Reg a, Reg b) {
        xchg(a.id(), b.id());
      }

      Reg to(Reg reg) const {
        return Reg::phys(_to[reg.id()]);
      }

      void reset() {
        for (size_t it = _swaps.size(); it-- > 0; ) {
          _builder.xchg64(Reg::phys(_swaps[it].first), Reg::phys(_swaps[it].second));
        }
        _swaps.clear();
      }
    };

    void peephole() {
      for (X86Block* block : _blocks) {
        for (auto it = block->begin(); it != block->end(); ) {
          X86Inst* inst = *it;
          if (is_noop(inst)) {
            it = it.erase();
          } else {
            if (((inst->kind() == X86Inst::Kind::Mov8Imm ||
                  inst->kind() == X86Inst::Kind::Mov16Imm ||
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
            } else if (inst->kind() == X86Inst::Kind::Div64) {
              ++it;

              RegPermutation perm(_builder);
              _builder.move_before(inst);
              perm.xchg(Reg::phys(0), perm.to(inst->misc()[1])); // RDX
              perm.xchg(Reg::phys(2), perm.to(inst->misc()[0])); // RAX
              inst->set_rm(perm.to(std::get<Reg>(inst->rm())));
              _builder.move_before(inst->next());
              perm.reset();
              continue;
            }

            ++it;
          }
        }
      }
    }

  public:
    X86CodeGen(Section* section, const std::vector<Reg>& input_pregs):
        Pass(section),
        _section(section),
        _builder(section->allocator(), nullptr),
        _input_pregs(input_pregs) {
      
      for (Reg reg : input_pregs) {
        _input_vregs.push_back(vreg());
      }

      section->autoname();
      _memory_deps.init(section);
      _isel.init(section);
      _vregs.init(section);

      _blocks.resize(section->size(), nullptr);
      for (Block* block : *section) {
        X86Block* x86_block = _builder.build_block();
        x86_block->set_name(block->name());
        _blocks[block->name()] = x86_block;
      }

      memory_deps();
      isel();
      autoname();
      regalloc();
      peephole();
      save("x86codegen.bin");
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

          if (mem.disp == 0) {
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

          if (mem.disp != 0) {
            if (mem.disp >= -128 && mem.disp <= 127) {
              byte(mem.disp & 0xff);
            } else {
              byte(mem.disp & 0xff);
              byte((mem.disp >> 8) & 0xff);
              byte((mem.disp >> 16) & 0xff);
              byte((mem.disp >> 24) & 0xff);
            }
          }
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
  };
}
