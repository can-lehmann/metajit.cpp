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

// Translation Validation for metajit.cpp

#include <z3++.h>

#include "jitir.hpp"

namespace metajit {
  namespace tv {
    class Z3CodeGen: public Pass<Z3CodeGen> {
    private:
      Section* _section;
      z3::context& _context;

      struct BlockData {
        z3::expr active;
        std::vector<z3::expr> args;

        BlockData(z3::context& context, Block* block):
            active(context.bool_val(false)) {
          for (Arg* arg : block->args()) {
            args.push_back(context.bv_val(0, type_width(arg->type())));
          }
        }
      };

      std::unordered_map<Block*, BlockData> _blocks;
      std::unordered_map<NamedValue*, z3::expr> _values;

    public:
      z3::expr emit(Value* value) {
        if (dynmatch(Const, constant, value)) {
          return _context.bv_val(constant->value(), type_width(constant->type()));
        } else if (value->is_named()) {
          return _values.at((NamedValue*) value);
        } else {
          assert(false && "Unknown value");
          return z3::expr(_context);
        }
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

      void enter(Block* block, Block* from, z3::expr enable, std::vector<z3::expr> args) {
        z3::expr taken = enable && _blocks.at(from).active;
        BlockData& block_data = _blocks.at(block);
        block_data.active = block_data.active || taken;
        for (size_t it = 0; it < args.size(); it++) {
          block_data.args[it] = z3::ite(taken, args[it], block_data.args[it]);
        }
      }

      z3::expr emit_inst(Inst* inst, Block* block) {
        if (dynamic_cast<FreezeInst*>(inst) || dynamic_cast<AssumeConstInst*>(inst)) {
          return emit(inst->arg(0));
        } else if (dynmatch(SelectInst, select, inst)) {
          z3::expr cond = emit(select->arg(0));
          z3::expr true_val = emit(select->arg(1));
          z3::expr false_val = emit(select->arg(2));
          return z3::ite(cond, true_val, false_val);
        } else if (dynmatch(ResizeUInst, resize_u, inst)) {
          return resize(emit(resize_u->arg(0)), type_width(resize_u->type()), false);
        } else if (dynmatch(ResizeSInst, resize_s, inst)) {
          return resize(emit(resize_s->arg(0)), type_width(resize_s->type()), true);
        } else if (dynmatch(ResizeXInst, resize_x, inst)) {
          return resize(emit(resize_x->arg(0)), type_width(resize_x->type()), false);
        } else if (dynmatch(LoadInst, load, inst)) {
          assert(false); // TODO
        } else if (dynmatch(StoreInst, store, inst)) {
          assert(false); // TODO
        } else if (dynmatch(AddPtrInst, add_ptr, inst)) {
          assert(false); // TODO
        }

        #define binop(InstType, expression) \
          else if (dynamic_cast<InstType*>(inst)) { \
            z3::expr a = emit(inst->arg(0)); \
            z3::expr b = emit(inst->arg(1)); \
            return expression; \
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
        binop(EqInst, a == b)
        binop(LtUInst, z3::ult(a, b))
        binop(LtSInst, z3::slt(a, b))

        #undef binop

        else if (dynmatch(BranchInst, branch, inst)) {
          z3::expr cond = emit(branch->arg(0));
          enter(branch->true_block(), block, cond, {});
          enter(branch->false_block(), block, !cond, {});
          return z3::expr(_context);
        } else if (dynmatch(JumpInst, jump, inst)) {
          std::vector<z3::expr> args;
          for (Value* arg : jump->args()) {
            args.push_back(emit(arg));
          }
          enter(jump->block(), block, _context.bool_val(true), args);
          return z3::expr(_context);
        } else if (dynmatch(ExitInst, exit, inst)) {
          return z3::expr(_context);
        } else if (dynmatch(CommentInst, comment, inst)) {
          return z3::expr(_context);
        } else {
          assert(false && "Unknown instruction");
          return z3::expr(_context);
        }
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
                std::vector<z3::expr> entry_args):
          Pass<Z3CodeGen>(section),
          _section(section),
          _context(context) {
        
        for (Block* block : *section) {
          _blocks.emplace(block, BlockData(context, block));
        }

        assert(entry_args.size() == section->entry()->args().size());

        _blocks.at(section->entry()).active = _context.bool_val(true);
        _blocks.at(section->entry()).args = entry_args;
        
        for (Block* block : *section) {
          emit_block(block);
        }
      }

    };
  }
}
