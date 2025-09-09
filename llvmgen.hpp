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
#include "llvm/IR/Module.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/IRBuilder.h"

#include "jitir.hpp"

#define dynmatch(Type, name, value) Type* name = dynamic_cast<Type*>(value)

namespace metajit {
  class LLVMCodeGen {
  private:
    Section* _section;
    llvm::LLVMContext& _context;
    llvm::Module* _module = nullptr;
    llvm::Function* _function = nullptr;
    llvm::IRBuilder<> _builder;

    std::unordered_map<Value*, llvm::Value*> _values;

    llvm::Type* emit_type(Type type) {
      switch (type) {
        case Type::Void: return llvm::Type::getVoidTy(_context);
        case Type::Bool: return llvm::Type::getInt1Ty(_context);
        case Type::Int8: return llvm::Type::getInt8Ty(_context);
        case Type::Int16: return llvm::Type::getInt16Ty(_context);
        case Type::Int32: return llvm::Type::getInt32Ty(_context);
        case Type::Int64: return llvm::Type::getInt64Ty(_context);
        case Type::Ptr: return llvm::PointerType::get(_context, 0);
      }
      assert(false && "Unknown type");
      return nullptr;
    }

    llvm::Value* emit_arg(Value* value) {
      return _values.at(value);
    }

    llvm::Value* emit_inst(Inst* inst) {
      if (dynmatch(ConstInst, constant, inst)) {
        return llvm::ConstantInt::get(
          emit_type(constant->type()),
          constant->value(),
          false
        );
      } else if (dynmatch(InputInst, input, inst)) {
        return _function->getArg(input->id());
      } else if (dynmatch(OutputInst, output, inst)) {
        llvm::Value* ptr = _function->getArg(_section->inputs().size() + output->id());
        _builder.CreateStore(emit_arg(output->arg(0)), ptr);
        return nullptr;
      } else if (dynmatch(SelectInst, select, inst)) {
        return _builder.CreateSelect(
          emit_arg(select->arg(0)),
          emit_arg(select->arg(1)),
          emit_arg(select->arg(2))
        );
      } else if (dynmatch(LoadInst, load, inst)) {
        return _builder.CreateLoad(
          emit_type(load->type()),
          emit_arg(load->arg(0))
        );
      } else if (dynmatch(StoreInst, store, inst)) {
        return _builder.CreateStore(
          emit_arg(store->arg(1)),
          emit_arg(store->arg(0))
        );
      } else if (dynmatch(AddPtrInst, addptr, inst)) {
        return _builder.CreateGEP(
          llvm::Type::getInt8Ty(_context),
          emit_arg(addptr->arg(0)),
          {emit_arg(addptr->arg(1))}
        );
      }

      #define binop(Name, LLVMName) \
        else if (dynmatch(Name##Inst, name, inst)) { \
          return _builder.Create##LLVMName( \
            emit_arg(name->arg(0)), \
            emit_arg(name->arg(1)) \
          ); \
        }

      binop(Add, Add)
      binop(Sub, Sub)
      binop(Mul, Mul)
      binop(And, And)
      binop(Or, Or)
      binop(Xor, Xor)
      binop(Shl, Shl)
      binop(ShrU, LShr)
      binop(ShrS, AShr)
      binop(Eq, ICmpEQ)
      binop(LtU, ICmpULT)
      binop(LtS, ICmpSLT)

      #undef binop

      assert(false && "Unknown instruction");
    }
  public:
    LLVMCodeGen(Section* section, llvm::Module* module):
        _section(section),
        _context(module->getContext()),
        _module(module),
        _builder(module->getContext()) {
      
      std::vector<llvm::Type*> args;
      for (Type type : _section->inputs()) {
        args.push_back(emit_type(type));
      }

      for (Type type : _section->outputs()) {
        args.push_back(llvm::PointerType::get(_context, 0));
      }

      _function = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(_context), args, false),
        llvm::Function::ExternalLinkage,
        "func",
        module
      );

      for (Block* block : *_section) {
        llvm::BasicBlock* llvm_block = llvm::BasicBlock::Create(_context, "block", _function);
        _builder.SetInsertPoint(llvm_block);

        for (Inst* inst : *block) {
          _values[inst] = emit_inst(inst);
        } 
      }
    }
  };
}
