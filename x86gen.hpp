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

  class RegArg: public Reg {
  private:
    bool _is_kill = false;
  public:
    using Reg::Reg;

    RegArg(const Reg& reg): Reg(reg) {}

    bool is_kill() const { return _is_kill; }
    RegArg& set_kill(bool is_kill = true) { _is_kill = is_kill; return *this; }

    void write(std::ostream& stream) const {
      Reg::write(stream);
      if (_is_kill) {
        stream << "<kill>";
      }
    }
  };
}

std::ostream& operator<<(std::ostream& stream, const metajit::Reg& reg) {
  reg.write(stream);
  return stream;
}

std::ostream& operator<<(std::ostream& stream, const metajit::RegArg& reg_arg) {
  reg_arg.write(stream);
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
      RegArg base;
      size_t scale = 0;
      RegArg index;
      int32_t disp = 0;

      Mem() {}
      Mem(RegArg _base): base(_base) {}
      Mem(RegArg _base, int32_t _disp): base(_base), disp(_disp) {}
      Mem(RegArg _base, size_t _scale, RegArg _index, int32_t _disp):
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

    using RM = std::variant<std::monostate, RegArg, Mem>;
    using Imm = std::variant<std::monostate, uint64_t, X86Block*>;
  private:
    Kind _kind;
    RegArg _reg;
    RM _rm;
    Imm _imm;
  public:
    X86Inst(Kind kind): _kind(kind) {}
    
    Kind kind() const { return _kind; }
    RegArg reg() const { return _reg; }
    RM rm() const { return _rm; }
    Imm imm() const { return _imm; }

    X86Inst& set_kind(Kind kind) { _kind = kind; return *this; }
    X86Inst& set_reg(RegArg reg) { _reg = reg; return *this; }
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

      if (std::holds_alternative<RegArg>(_rm)) {
        fn(std::get<RegArg>(_rm));
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
        if (std::holds_alternative<RegArg>(rm)) {
          use_fn(std::get<RegArg>(rm));
        } else if (std::holds_alternative<Mem>(rm)) {
          Mem mem = std::get<Mem>(rm);
          use_fn(mem.base);
          if (!mem.index.is_invalid()) {
            use_fn(mem.index);
          }
        }
      };

      auto def = [&](RM rm) {
        if (std::holds_alternative<RegArg>(rm)) {
          def_fn(std::get<RegArg>(rm));
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

    void insert_before(X86Inst* before, X86Inst* inst) {
      _insts.insert_before(before, inst);
    }

    void write(std::ostream& stream) {
      stream << "b" << _name << ":\n";
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

    if (std::holds_alternative<RegArg>(_rm)) {
      stream << " rm=" << std::get<RegArg>(_rm);
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
  };

  class X86CodeGen: public Pass<X86CodeGen> {
  private:
    Section* _section;
    std::vector<X86Block*> _blocks;
    X86InstBuilder _builder;

    InstMap<std::vector<X86Inst>> _isel;

    InstMap<Reg> _vregs;
    std::vector<Reg> _input_pregs;
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
      } else if (dynmatch(Input, input, value)) {
        return _input_pregs[input->index()];
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
            default:
              assert(false && "Unsupported store type");
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
        _builder.div(vreg(inst->arg(1)));
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
        return std::holds_alternative<RegArg>(inst->rm());
      }
      return false;
    }

    void regalloc() {
      std::vector<Reg> mapping(_next_vreg, Reg());
      PhysicalRegSet available_regs;

      std::vector<bool> used_vregs(_next_vreg, false);
      std::vector<bool> used_pregs(16, false);
      for (size_t block_it = _blocks.size(); block_it-- > 0; ) {
        X86Block* block = _blocks[block_it];
        for (X86Inst* inst : block->rev_range()) {
          inst->visit_regs([&](RegArg& reg) {
            if (reg.is_virtual()) {
              if (!used_vregs[reg.id()]) {
                used_vregs[reg.id()] = true;
                reg.set_kill(true);
              }
            } else if (reg.is_physical()) {
              if (!used_pregs[reg.id()]) {
                used_pregs[reg.id()] = true;
                reg.set_kill(true);
              }
            }
          });
        }
      }

      for (Reg reg : _input_pregs) {
        assert(reg.is_physical());
        available_regs.erase(reg.id());
      }

      for (X86Block* block : _blocks) {
        for (X86Inst* inst : *block) {
          if (is_reg_mov(inst)) {
            RegArg src_reg = std::get<RegArg>(inst->rm());
            RegArg dst_reg = inst->reg();
            if (src_reg.is_virtual() &&
                dst_reg.is_virtual() &&
                src_reg.is_kill()) {
              mapping[dst_reg.id()] = mapping[src_reg.id()];
              src_reg.set_kill(false);
              inst->set_rm(src_reg);
            }
          }

          inst->visit_regs([&](RegArg& reg) {
            if (reg.is_virtual()) {
              if (mapping[reg.id()].is_invalid()) {
                Reg phys_reg = available_regs.get_any();
                assert(!phys_reg.is_invalid() && "Ran out of physical registers");
                mapping[reg.id()] = phys_reg;
                available_regs.erase(phys_reg.id());
              }

              RegArg phys_reg = mapping[reg.id()];
              phys_reg.set_kill(reg.is_kill());
              reg = phys_reg;
            }
          });

          inst->visit_regs([&](RegArg& reg) {
            assert(reg.is_physical());
            if (reg.is_kill()) {
              available_regs.insert(reg.id());
            }
          });
        }
      }
    }

    bool is_noop(X86Inst* inst) {
      if (is_reg_mov(inst)) {
        if (std::holds_alternative<RegArg>(inst->rm())) {
          Reg src_reg = std::get<RegArg>(inst->rm());
          return src_reg == inst->reg();
        }
      }
      return false;
    }

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
                std::holds_alternative<RegArg>(inst->rm())) ||
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
                    inst->set_reg(std::get<RegArg>(inst->rm()));
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
    }

  public:
    X86CodeGen(Section* section, const std::vector<Reg>& input_pregs):
        Pass(section),
        _section(section),
        _builder(section->allocator(), nullptr),
        _input_pregs(input_pregs) {
      
      section->autoname();
      _isel.init(section);
      _vregs.init(section);

      _blocks.resize(section->size(), nullptr);
      for (Block* block : *section) {
        X86Block* x86_block = _builder.build_block();
        x86_block->set_name(block->name());
        _blocks[block->name()] = x86_block;
      }

      isel();
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
        if (std::holds_alternative<RegArg>(rm)) {
          rex |= ((std::get<RegArg>(rm).id() >> 3) & 1); // B
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
        } else if (std::holds_alternative<RegArg>(rm)) {
          if (std::get<RegArg>(rm).id() >= 8) {
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
        if (std::holds_alternative<RegArg>(rm)) {
          modrm |= 0b11 << 6; // mod
          modrm |= std::get<RegArg>(rm).id() & 0b111; // r/m
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
  };
}
