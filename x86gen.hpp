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
    };

    using RM = std::variant<std::monostate, Reg, Mem>;
    using Imm = std::variant<std::monostate, uint64_t, X86Inst*>;
  private:
    Kind _kind;
    Reg _reg;
    RM _rm;
    Imm _imm;
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

    void write(std::ostream& stream, bool usedef = true) const {
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
      } else if (std::holds_alternative<X86Inst*>(_imm)) {
        stream << " imm=<INST>";
      }

      if (usedef) {
        stream << "\t;";

        auto use = [&](Reg reg){ stream << " use " << reg; };
        auto def = [&](Reg reg){ stream << " def " << reg; };
        visit_usedef(use, def);
      }
    }
  };

  class X86InstBuilder {
  private:
    Allocator& _allocator;
    LinkedList<X86Inst>& _list;
    X86Inst* _insert_pos = nullptr;

    X86Inst& build(X86Inst::Kind kind) {
      X86Inst* inst = (X86Inst*) _allocator.alloc(sizeof(X86Inst), alignof(X86Inst));
      new (inst) X86Inst(kind);
      _list.insert_before(_insert_pos, inst);
      return *inst;
    }
  public:
    X86InstBuilder(Allocator& allocator, LinkedList<X86Inst>& list):
      _allocator(allocator),
      _list(list),
      _insert_pos(nullptr) {}
    
    void move_before(X86Inst* inst) {
      _insert_pos = inst;
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

    #include "x86insts.inc.hpp"

    X86Inst* mov64_imm64(Reg dst, X86Inst::Imm imm) {
      return &build(X86Inst::Kind::Mov64Imm).set_reg(dst).set_imm(imm);
    }
    
    X86Inst* lea64(Reg dst, X86Inst::Mem src) {
      return &build(X86Inst::Kind::Lea64).set_reg(dst).set_rm(src);
    }

    X86Inst* sete8(X86Inst::RM dst) {
      return &build(X86Inst::Kind::SetE8).set_rm(dst);
    }

    X86Inst* setl8(X86Inst::RM dst) {
      return &build(X86Inst::Kind::SetL8).set_rm(dst);
    }

    X86Inst* setb8(X86Inst::RM dst) {
      return &build(X86Inst::Kind::SetB8).set_rm(dst);
    }

    X86Inst* ret() {
      return &build(X86Inst::Kind::Ret);
    }
  };

  class X86CodeGen: public Pass<X86CodeGen> {
  private:
    Section* _section;
    LinkedList<X86Inst> _insts;
    X86InstBuilder _builder;

    InstMap<std::vector<X86Inst>> _isel;

    InstMap<Reg> _vregs;
    size_t _next_vreg = 0;

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
      } 

      if (mem.base.is_invalid()) {
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
      } else if (dynmatch(InputInst, input, inst)) {
      } else if (dynmatch(OutputInst, input, inst)) {
      } else if (dynmatch(SelectInst, select, inst)) {
        _builder.mov64(vreg(inst), vreg(select->arg(1)));

        if (dynmatch(Inst, pred_inst, select->arg(0))) {
          if (dynamic_cast<EqInst*>(pred_inst) ||
              dynamic_cast<LtSInst*>(pred_inst) ||
              dynamic_cast<LtUInst*>(pred_inst)) {
            build_cmp(pred_inst->arg(0), pred_inst->arg(1));
            if (dynamic_cast<EqInst*>(pred_inst)) {
              _builder.cmove64(vreg(inst), vreg(select->arg(2)));
            } else if (dynamic_cast<LtSInst*>(pred_inst)) {
              _builder.cmovl64(vreg(inst), vreg(select->arg(2)));
            } else if (dynamic_cast<LtUInst*>(pred_inst)) {
              _builder.cmovb64(vreg(inst), vreg(select->arg(2)));
            } else {
              assert(false);
            }
            return;
          }
        }
        
        _builder.test64(vreg(select->arg(0)), vreg(select->arg(0)));
        _builder.cmovz64(vreg(inst), vreg(select->arg(2)));
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
      } else if (dynmatch(ModSInst, mod_s, inst)) {
        assert(false && "Not implemented");
      } else if (dynmatch(ModUInst, mod_u, inst)) {
        assert(false && "Not implemented");
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
      } else if (dynmatch(ShrUInst, shr_u, inst)) {
        _builder.mov64(vreg(inst), vreg(xor_inst->arg(0)));
        _builder.shr64(vreg(inst), vreg(xor_inst->arg(1)));
      } else if (dynmatch(ShrSInst, shr_s, inst)) {
        _builder.mov64(vreg(inst), vreg(xor_inst->arg(0)));
        _builder.sar64(vreg(inst), vreg(xor_inst->arg(1)));
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
        assert(false && "Not implemented");
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
        for (Inst* inst : block->rev_range()) {
          _builder.move_before(_insts.first());
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

    void regalloc() {
      std::vector<Reg> mapping(_next_vreg, Reg());
      PhysicalRegSet available_regs;

      for (X86Inst* inst : _insts.rev_range()) {
        if (is_reg_mov(inst)) {
          Reg src_reg = std::get<Reg>(inst->rm());
          Reg dst_reg = inst->reg();
          if (src_reg.is_virtual() &&
              dst_reg.is_virtual() &&
              mapping[src_reg.id()].is_invalid()) {
            mapping[src_reg.id()] = mapping[dst_reg.id()];
            continue;
          }
        }

        auto def = [&](Reg reg){
          size_t id = reg.id();
          if (reg.is_virtual()) {
            id = mapping[id].id();
          }
          available_regs.insert(id);
        };

        inst->visit_usedef([&](Reg reg){}, def);

        auto use_physical = [&](Reg reg){
          if (reg.is_physical()) {
            available_regs.erase(reg.id());
          } else if (reg.is_virtual()) {
            if (!mapping[reg.id()].is_invalid()) {
              available_regs.erase(mapping[reg.id()].id());
            }
          }
        };

        inst->visit_usedef(use_physical, [&](Reg reg){});

        auto use_virtual = [&](Reg reg){
          if (reg.is_virtual() && mapping[reg.id()].is_invalid()) {
            Reg physical_reg = available_regs.get_any();
            assert(!physical_reg.is_invalid());
            mapping[reg.id()] = physical_reg;
            available_regs.erase(physical_reg.id());
          }
        };

        inst->visit_usedef(use_virtual, [&](Reg reg){});
      }

      for (X86Inst* inst : _insts) {
        inst->visit_regs([&](Reg& reg){
          if (reg.is_virtual()) {
            reg = mapping[reg.id()];
          }
        });
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

    void peephole() {
      for (auto it = _insts.begin(); it != _insts.end(); ) {
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
          }
          ++it;
        }
      }
    }

  public:
    X86CodeGen(Section* section):
        Pass(section),
        _section(section),
        _builder(section->allocator(), _insts) {
      
      section->autoname();
      _isel.init(section);
      _vregs.init(section);

      isel();
      regalloc();
      write(std::cout);
      save("x86codegen.bin");
      peephole();
    }

    void emit(std::vector<uint8_t>& buffer) {
      for (X86Inst* inst : _insts) {
        Reg reg = inst->reg();
        X86Inst::RM rm = inst->rm();
        std::optional<uint64_t> imm;
        if (std::holds_alternative<uint64_t>(inst->imm())) {
          imm = std::get<uint64_t>(inst->imm());
        } else if (std::holds_alternative<X86Inst*>(inst->imm())) {
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
      for (X86Inst* inst : _insts) {
        inst->write(stream);
        stream << std::endl;
      }
    }
  };
}
