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
#include "llvm/IR/GetElementPtrTypeIterator.h"

#include "jitir.hpp"

namespace metajit {
  class LowerLLVM {
  private:
    llvm::Function* _function = nullptr;
    const llvm::DataLayout& _data_layout;
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

    Value* lower_operand(llvm::Value* value) {
      if (llvm::ConstantInt* constant_int = llvm::dyn_cast<llvm::ConstantInt>(value)) {
        Type type = lower_type(constant_int->getType());
        return _builder.build_const(type, constant_int->getZExtValue());
      } else if (_values.find(value) != _values.end()) {
        return _values.at(value);
      } else {
        value->print(llvm::errs());
        llvm::errs() << "\n";
        
        assert(false && "Unable to lower operand");
        return nullptr;
      }
    }

    void lower_jump(llvm::BasicBlock* from, llvm::BasicBlock* to) {
      Block* lowered_to = _blocks.at(to);
      std::vector<Value*> args;
      for (llvm::PHINode& phi : to->phis()) {
        llvm::Value* value = phi.getIncomingValueForBlock(from);
        args.push_back(lower_operand(value));
      }
      _builder.build_jump(lowered_to, args);
    }

    Block* lower_jump_if_required(llvm::BasicBlock* from, llvm::BasicBlock* to) {
      // Only jump instructions can pass block arguments.
      Block* lowered_to = _blocks.at(to);
      if (lowered_to->args().size() == 0) {
        return lowered_to;
      } else {
        Block* current_block = _builder.block();
        Inst* current_before = _builder.before();

        Block* jump_block = _builder.build_block_before(current_block->next());
        _builder.move_to_end(jump_block);
        lower_jump(from, to);
        
        _builder.move_to(current_block, current_before);
        return jump_block;
      }
    }

    Value* lower_intrinsic(llvm::StringRef name, std::vector<llvm::Value*> args, Type return_type) {
      if (name.starts_with("__metajit_freeze")) {
        assert(args.size() == 1);
        return _builder.build_freeze(lower_operand(args[0]));
      } else if (name.starts_with("__metajit_assume_const")) {
        assert(args.size() == 1);
        return _builder.build_assume_const(lower_operand(args[0]));
      } else if (name.starts_with("__metajit_load_pure")) {
        assert(args.size() == 1);
        return _builder.build_load(
          lower_operand(args[0]),
          return_type,
          LoadFlags::Pure,
          AliasingGroup(0),
          0
        );
      } else if (name.starts_with("__metajit_comment")) {
        llvm::GlobalVariable* global_var = llvm::cast<llvm::GlobalVariable>(args[0]);
        llvm::ConstantDataArray* constant_data_array = llvm::dyn_cast<llvm::ConstantDataArray>(global_var->getInitializer());
        std::string text = constant_data_array->getAsCString().str();
        return _builder.build_comment(text);
      } else {
        assert(false && "Unknown intrinsic");
        return nullptr;
      }
    }

    Value* lower_inst(llvm::Instruction* inst) {
      #define fail_lowering(message) {\
        inst->print(llvm::errs()); \
        llvm::errs() << "\n"; \
        assert(false && message); \
        return nullptr; \
      }
      
      if (llvm::ICmpInst* icmp = llvm::dyn_cast<llvm::ICmpInst>(inst)) {
        Value* a = lower_operand(icmp->getOperand(0));
        Value* b = lower_operand(icmp->getOperand(1));

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
          default: fail_lowering("Unknown ICmp predicate");
        }
      } else if (llvm::BinaryOperator* binop = llvm::dyn_cast<llvm::BinaryOperator>(inst)) {
        Value* a = lower_operand(binop->getOperand(0));
        Value* b = lower_operand(binop->getOperand(1));

        switch (binop->getOpcode()) {
          case llvm::Instruction::Add: return _builder.fold_add(a, b);
          case llvm::Instruction::Sub: return _builder.fold_sub(a, b);
          case llvm::Instruction::Mul: return _builder.fold_mul(a, b);
          case llvm::Instruction::UDiv: return _builder.fold_div_u(a, b);
          case llvm::Instruction::SDiv: return _builder.fold_div_s(a, b);
          case llvm::Instruction::URem: return _builder.fold_mod_u(a, b);
          case llvm::Instruction::SRem: return _builder.fold_mod_s(a, b);
          case llvm::Instruction::Shl: return _builder.fold_shl(a, b);
          case llvm::Instruction::AShr: return _builder.fold_shr_s(a, b);
          case llvm::Instruction::LShr: return _builder.fold_shr_u(a, b);
          case llvm::Instruction::And: return _builder.fold_and(a, b);
          case llvm::Instruction::Or: return _builder.fold_or(a, b);
          case llvm::Instruction::Xor: return _builder.fold_xor(a, b);
          default: fail_lowering("Unknown binary operator");
        }
      } else if (llvm::CastInst* cast = llvm::dyn_cast<llvm::CastInst>(inst)) {
        Value* a = lower_operand(cast->getOperand(0));
        Type type = lower_type(cast->getType());
        
        switch (cast->getOpcode()) {
          case llvm::Instruction::ZExt: return _builder.fold_resize_u(a, type);
          case llvm::Instruction::Trunc: return _builder.fold_resize_u(a, type);
          case llvm::Instruction::SExt: return _builder.fold_resize_s(a, type);
          default: fail_lowering("Unknown cast instruction");
        }
      } else if (llvm::SelectInst* select = llvm::dyn_cast<llvm::SelectInst>(inst)) {
        Value* cond = lower_operand(select->getCondition());
        Value* true_value = lower_operand(select->getTrueValue());
        Value* false_value = lower_operand(select->getFalseValue());
        return _builder.fold_select(cond, true_value, false_value);
      } else if (llvm::LoadInst* load = llvm::dyn_cast<llvm::LoadInst>(inst)) {
        Value* ptr = lower_operand(load->getPointerOperand());
        Type type = lower_type(load->getType());
        return _builder.fold_load(ptr, type, LoadFlags::None, AliasingGroup(0), 0);
      } else if (llvm::StoreInst* store = llvm::dyn_cast<llvm::StoreInst>(inst)) {
        Value* ptr = lower_operand(store->getPointerOperand());
        Value* value = lower_operand(store->getValueOperand());
        return _builder.fold_store(ptr, value, AliasingGroup(0), 0);
      } else if (llvm::GetElementPtrInst* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(inst)) {
        Value* ptr = lower_operand(gep->getPointerOperand());
        assert(ptr->type() == Type::Ptr);
        for (auto it = llvm::gep_type_begin(gep), end = llvm::gep_type_end(gep); it != end; ++it) {
          Value* index = _builder.fold_resize_u(lower_operand(it.getOperand()), Type::Int64);
          uint64_t stride = it.getSequentialElementStride(_data_layout).getFixedValue();
          if (stride != 1) {
            index = _builder.fold_mul(index, _builder.build_const(Type::Int64, stride));
          }
          ptr = _builder.fold_add_ptr(ptr, index);
        }
        return ptr;
      } else if (llvm::PHINode* phi = llvm::dyn_cast<llvm::PHINode>(inst)) {
        assert(_values.find(phi) != _values.end()); // PHI nodes are handled in lower_block
        return _values.at(phi);
      } else if (llvm::BranchInst* branch = llvm::dyn_cast<llvm::BranchInst>(inst)) {
        llvm::BasicBlock* from = branch->getParent();
        if (branch->isUnconditional()) {
          lower_jump(from, branch->getSuccessor(0));
        } else {
          Value* cond = lower_operand(branch->getCondition());
          _builder.fold_branch(
            cond,
            lower_jump_if_required(from, branch->getSuccessor(0)),
            lower_jump_if_required(from, branch->getSuccessor(1))
          );
        }
        return nullptr;
      } else if (llvm::ReturnInst* ret = llvm::dyn_cast<llvm::ReturnInst>(inst)) {
        return _builder.build_exit();
      } else if (llvm::CallInst* call = llvm::dyn_cast<llvm::CallInst>(inst)) {
        if (call->getCalledFunction() && call->getCalledFunction()->getName().starts_with("__metajit")) {
          llvm::StringRef name = call->getCalledFunction()->getName();
          std::vector<llvm::Value*> args;
          args.insert(args.end(), call->arg_begin(), call->arg_end());
          Type return_type = lower_type(call->getType());
          return lower_intrinsic(name, args, return_type);
        } else {
          fail_lowering("Unable to lower call instruction");
        }
      } else {
        fail_lowering("Unable to lower instruction");
      }

      #undef fail_lowering
    }

    Block* lower_block(llvm::BasicBlock* block) {
      std::vector<Type> arg_types;
      std::vector<llvm::Value*> phis;
      for (llvm::PHINode& phi : block->phis()) {
        phis.push_back(&phi);
        arg_types.push_back(lower_type(phi.getType()));
      }
      Block* result = _builder.build_block(arg_types);
      for (size_t it = 0; it < phis.size(); it++) {
        _values[phis[it]] = result->arg(it);
      }
      return result;
    }
  public:
    LowerLLVM(llvm::Function* function, Section* section):
        _function(function),
        _data_layout(function->getParent()->getDataLayout()),
        _section(section),
        _builder(section) {
      
      std::vector<Type> entry_arg_types;
      for (llvm::Argument& arg : function->args()) {
        entry_arg_types.push_back(lower_type(arg.getType()));
      }
      Block* entry_block = _builder.build_block(entry_arg_types);
      for (llvm::Argument& arg : function->args()) {
        _values[&arg] = _builder.entry_arg(arg.getArgNo());
      }

      for (llvm::BasicBlock& block : *function) {
        _blocks[&block] = lower_block(&block);
      }

      _builder.move_to_end(entry_block);
      _builder.fold_jump(_blocks.at(&function->getEntryBlock()));

      for (llvm::BasicBlock& block : *function) {
        _builder.move_to_end(_blocks.at(&block));

        for (llvm::Instruction& inst : block) {
          _values[&inst] = lower_inst(&inst);
        }
      }
    }
  };
}
