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

#include <unordered_map>

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Constants.h"

#include "jitir.hpp"

namespace metajit {
  class LowerLLVM {
  private:
    llvm::Function* _function = nullptr;
    Section* _section = nullptr;
    Builder _builder;

    std::unordered_map<llvm::BasicBlock*, Block*> _blocks;
    std::unordered_map<llvm::Value*, Value*> _values;

    Type lower_type(llvm::Type* type) {
      if (type->isVoidTy()) {
        return Type::Void;
      } else if (type->isIntegerTy(1)) {
        return Type::Bool;
      } else if (type->isIntegerTy(8)) {
        return Type::Int8;
      } else if (type->isIntegerTy(16)) {
        return Type::Int16;
      } else if (type->isIntegerTy(32)) {
        return Type::Int32;
      } else if (type->isIntegerTy(64)) {
        return Type::Int64;
      } else if (type->isPointerTy()) {
        return Type::Ptr;
      } else {
        assert(false && "Unable to lower type");
        return Type::Void;
      }
    }

    Value* lower_arg(llvm::Value* value) {
      if (llvm::ConstantInt* constant_int = llvm::dyn_cast<llvm::ConstantInt>(value)) {
        Type type = lower_type(constant_int->getType());
        return _builder.build_const(type, constant_int->getZExtValue());
      } else if (_values.find(value) != _values.end()) {
        return _values.at(value);
      } else {
        value->print(llvm::errs());
        llvm::errs() << "\n";
        
        assert(false && "Unable to lower argument");
        return nullptr;
      }
    }

    Value* lower_inst(llvm::Instruction* inst) {
      if (llvm::ICmpInst* icmp = llvm::dyn_cast<llvm::ICmpInst>(inst)) {
        Value* a = lower_arg(icmp->getOperand(0));
        Value* b = lower_arg(icmp->getOperand(1));

        switch (icmp->getPredicate()) {
          case llvm::ICmpInst::ICMP_EQ: return _builder.fold_eq(a, b);
          case llvm::ICmpInst::ICMP_NE: return _builder.fold_ne(a, b);
          case llvm::ICmpInst::ICMP_UGT: return _builder.fold_gt_u(a, b);
          case llvm::ICmpInst::ICMP_UGE: return _builder.fold_ge_u(a, b);
          case llvm::ICmpInst::ICMP_ULT: return _builder.fold_lt_u(a, b);
          case llvm::ICmpInst::ICMP_ULE: return _builder.fold_le_u(a, b);
          case llvm::ICmpInst::ICMP_SGT: return _builder.fold_gt_s(a, b);
          case llvm::ICmpInst::ICMP_SGE: return _builder.fold_ge_s(a, b);
          case llvm::ICmpInst::ICMP_SLT: return _builder.fold_lt_s(a, b);
          case llvm::ICmpInst::ICMP_SLE: return _builder.fold_le_s(a, b);
          default:
            assert(false && "Unknown icmp predicate");
            return nullptr;
        }
      } else if (llvm::LoadInst* load = llvm::dyn_cast<llvm::LoadInst>(inst)) {
        Value* ptr = lower_arg(load->getPointerOperand());
        Type type = lower_type(load->getType());
        return _builder.build_load(ptr, type, LoadFlags::None, AliasingGroup(0), 0);
      } else if (llvm::StoreInst* store = llvm::dyn_cast<llvm::StoreInst>(inst)) {
        Value* ptr = lower_arg(store->getPointerOperand());
        Value* value = lower_arg(store->getValueOperand());
        return _builder.build_store(ptr, value, AliasingGroup(0), 0);
      } else {
        inst->print(llvm::errs());
        llvm::errs() << "\n";

        assert(false && "Unable to lower instruction");
        return nullptr;
      }
    }
  public:
    LowerLLVM(llvm::Function* function, Section* section):
        _function(function), _section(section), _builder(section) {
      
      for (llvm::BasicBlock& block : *function) {
        _blocks[&block] = _builder.build_block();
      }

      for (llvm::Argument& arg : function->args()) {
        _values[&arg] = _builder.build_input(lower_type(arg.getType()));
      }

      for (llvm::BasicBlock& block : *function) {
        _builder.move_to_end(_blocks.at(&block));

        for (llvm::Instruction& inst : block) {
          _values[&inst] = lower_inst(&inst);
        }
      }
    }
  };
}
