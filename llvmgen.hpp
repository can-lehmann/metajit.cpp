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

#include <map>
#include <unordered_map>
#include <queue>
#include <fstream>

#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/PassManager.h"

#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Passes/PassBuilder.h"

#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"

#include "llvm/Support/TargetSelect.h"

#include "jitir.hpp"
#include "jitir_llvmapi.hpp"
#include "genext.hpp"

namespace metajit {
  class LLVMCodeGen: public Pass<LLVMCodeGen> {
  private:
    bool _comments = false; // Emit comments as references back to original IR

    Section* _section;
    llvm::LLVMContext& _context;
    llvm::Module* _module = nullptr;
    LLVM_API _llvm_api;
    llvm::Function* _function = nullptr;
    llvm::IRBuilder<> _builder;

    std::map<Block*, llvm::BasicBlock*> _blocks;
    std::map<Block*, llvm::BasicBlock*> _end_blocks;

    NameMap<llvm::Value*> _values;

    void emit_branch(llvm::Value* cond,
                     const std::function<void()>& emit_then,
                     const std::function<void()>& emit_else,
                     llvm::Twine name = "") {

      llvm::BasicBlock* then_block = llvm::BasicBlock::Create(_context, name + "then", _function);
      llvm::BasicBlock* else_block = llvm::BasicBlock::Create(_context, name + "else", _function);
      llvm::BasicBlock* cont_block = llvm::BasicBlock::Create(_context, name + "cont", _function);

      _builder.CreateCondBr(cond, then_block, else_block);

      _builder.SetInsertPoint(then_block);
      emit_then();
      _builder.CreateBr(cont_block);

      _builder.SetInsertPoint(else_block);
      emit_else();
      _builder.CreateBr(cont_block);

      _builder.SetInsertPoint(cont_block);
    }

    llvm::Type* emit_type(Type type) {
      switch (type) {
        case Type::Void: return llvm::Type::getVoidTy(_context);
        case Type::Bool: return llvm::Type::getInt1Ty(_context);
        case Type::Int8: return llvm::Type::getInt8Ty(_context);
        case Type::Int16: return llvm::Type::getInt16Ty(_context);
        case Type::Int32: return llvm::Type::getInt32Ty(_context);
        case Type::Int64: return llvm::Type::getInt64Ty(_context);
        case Type::Float32: return llvm::Type::getFloatTy(_context);
        case Type::Float64: return llvm::Type::getDoubleTy(_context);
        case Type::Ptr: return llvm::PointerType::get(_context, 0);
      }
      assert(false && "Unknown type");
      return nullptr;
    }

    llvm::Value* emit_arg(Value* value) {
      if (dynmatch(Const, constant, value)) {
        if (constant->type() == Type::Ptr) {
          llvm::Value* addr = llvm::ConstantInt::get(
            llvm::Type::getInt64Ty(_context),
            constant->value(),
            false
          );
          return llvm::ConstantExpr::getIntToPtr(
            (llvm::Constant*) addr,
            llvm::PointerType::get(_context, 0)
          );
        }
        return llvm::ConstantInt::get(
          emit_type(constant->type()),
          constant->value(),
          false
        );
      } else if (dynmatch(Symbol, symbol, value)) {
        std::string name(symbol->symbol().data(), symbol->symbol().size());
        auto it = _llvm_api.by_name.find(name);
        assert(it != _llvm_api.by_name.end() && "Unknown symbol");
        return _builder.CreateBitOrPointerCast(it->second, llvm::PointerType::get(_context, 0));
      } else if (value->is_named()) {
        return _values.at((NamedValue*) value);
      } else {
        assert(false && "Unknown value");
        return nullptr;
      }
    }

    llvm::Value* emit_add_offset(llvm::Value* ptr, uint64_t offset) {
      if (offset == 0) {
        return ptr;
      } else {
        return _builder.CreateGEP(
          llvm::Type::getInt8Ty(_context),
          ptr,
          {llvm::ConstantInt::get(llvm::Type::getInt64Ty(_context), offset)}
        );
      }
    }

    llvm::Value* emit_phi(Arg* arg) {
      return _builder.CreatePHI(emit_type(arg->type()), 0);
    }

    llvm::Value* emit_inst(Inst* inst) {
      if (dynmatch(FreezeInst, freeze, inst)) {
        return _builder.CreateFreeze(emit_arg(freeze->arg(0)));
      } else if (dynmatch(PromoteInst, promote, inst)) {
        return emit_arg(promote->arg(0));
      } else if (dynmatch(AssumeConstInst, assume_const, inst)) {
        return emit_arg(assume_const->arg(0));
      } else if (dynmatch(SelectInst, select, inst)) {
        return _builder.CreateSelect(
          emit_arg(select->arg(0)),
          emit_arg(select->arg(1)),
          emit_arg(select->arg(2))
        );
      } else if (dynmatch(ResizeUInst, resize_u, inst)) {
        return _builder.CreateZExtOrTrunc(
          emit_arg(resize_u->arg(0)),
          emit_type(resize_u->type())
        );
      } else if (dynmatch(ResizeSInst, resize_s, inst)) {
        return _builder.CreateSExtOrTrunc(
          emit_arg(resize_s->arg(0)),
          emit_type(resize_s->type())
        );
      } else if (dynmatch(ResizeXInst, resize_x, inst)) {
        return _builder.CreateZExtOrTrunc(
          emit_arg(resize_x->arg(0)),
          emit_type(resize_x->type())
        );
      } else if (dynmatch(FloatToIntSInst, float_to_int_s, inst)) {
        return _builder.CreateFPToSI(
          emit_arg(float_to_int_s->arg(0)),
          emit_type(float_to_int_s->type())
        );
      } else if (dynmatch(IntToFloatSInst, int_to_float_s, inst)) {
        return _builder.CreateSIToFP(
          emit_arg(int_to_float_s->arg(0)),
          emit_type(int_to_float_s->type())
        );
      } else if (dynmatch(PtrToIntInst, ptr_to_int, inst)) {
        return _builder.CreatePtrToInt(
          emit_arg(ptr_to_int->arg(0)),
          emit_type(ptr_to_int->type())
        );
      } else if (dynmatch(LoadInst, load, inst)) {
        return _builder.CreateLoad(
          emit_type(load->type()),
          emit_add_offset(emit_arg(load->arg(0)), load->offset())
        );
      } else if (dynmatch(StoreInst, store, inst)) {
        return _builder.CreateStore(
          emit_arg(store->arg(1)),
          emit_add_offset(emit_arg(store->arg(0)), store->offset())
        );
      } else if (dynmatch(AllocaInst, alloca, inst)) {
        llvm::AllocaInst* llvm_alloca = _builder.CreateAlloca(
          llvm::Type::getInt8Ty(_context),
          emit_arg(alloca->size())
        );
        llvm_alloca->setAlignment(llvm::Align(alloca->align()));
        return llvm_alloca;
      } else if (dynmatch(AddPtrInst, add_ptr, inst)) {
        return _builder.CreateGEP(
          llvm::Type::getInt8Ty(_context),
          emit_arg(add_ptr->arg(0)),
          {emit_arg(add_ptr->arg(1))}
        );
      } else if (dynmatch(CallInst, call, inst)) {
        std::vector<llvm::Type*> arg_types;
        std::vector<llvm::Value*> args;
        for (size_t it = 1; it < call->arg_count(); it++) {
          Value* arg = call->arg(it);
          arg_types.push_back(emit_type(arg->type()));
          args.push_back(emit_arg(arg));
        }

        llvm::FunctionType* function_type = llvm::FunctionType::get(
          emit_type(call->type()),
          arg_types,
          false
        );

        llvm::CallInst* call_inst = _builder.CreateCall(
          function_type,
          emit_arg(call->callee()),
          args
        );

        if (call->call_conv() == CallConv::PreserveNone) {
          call_inst->setCallingConv(llvm::CallingConv::PreserveNone);
        }

        return call_inst;
      } else if (dynmatch(EqInst, eq, inst)) {
        if (is_float(eq->arg(0)->type())) {
          return _builder.CreateFCmpUEQ(
            emit_arg(eq->arg(0)),
            emit_arg(eq->arg(1))
          );
        } else {
          return _builder.CreateICmpEQ(
            emit_arg(eq->arg(0)),
            emit_arg(eq->arg(1))
          );
        }
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
      binop(DivS, SDiv)
      binop(DivU, UDiv)
      binop(ModS, SRem)
      binop(ModU, URem)
      binop(And, And)
      binop(Or, Or)
      binop(Xor, Xor)
      binop(Shl, Shl)
      binop(ShrU, LShr)
      binop(ShrS, AShr)
      binop(AddF, FAdd)
      binop(SubF, FSub)
      binop(MulF, FMul)
      binop(DivF, FDiv)
      binop(LtU, ICmpULT)
      binop(LtS, ICmpSLT)
      binop(LtFO, FCmpOLT)
      binop(LtFU, FCmpULT)

      #undef binop

      else if (dynmatch(CommentInst, comment, inst)) {
        return nullptr;
      }

      inst->write(std::cerr);
      std::cerr << std::endl;
      assert(false && "Unknown instruction");
      return nullptr;
    }

    llvm::Value* emit_terminator(Inst* inst, Block* block) {
      if (dynmatch(BranchInst, branch, inst)) {
        return _builder.CreateCondBr(
          emit_arg(branch->cond()),
          _blocks.at(branch->true_block()),
          _blocks.at(branch->false_block())
        );
      } else if (dynmatch(JumpInst, jump, inst)) {
        for (Arg* arg : jump->block()->args()) {
          llvm::PHINode* phi = (llvm::PHINode*) _values.at(arg);
          phi->addIncoming(emit_arg(jump->arg(arg->index())), _builder.GetInsertBlock());
        }

        return _builder.CreateBr(_blocks.at(jump->block()));
      } else if (dynmatch(ExitInst, exit, inst)) {
        return _builder.CreateRetVoid();
      } else {
        assert(false && "Unknown terminator");
        return nullptr;
      }
    }

  public:
    LLVMCodeGen(Section* section,
                llvm::Module* module,
                const std::string& name):
        Pass(section),
        _section(section),
        _context(module->getContext()),
        _module(module),
        _llvm_api(module),
        _builder(module->getContext()) {
            
      assert(_section->ordering() >= BlockOrdering::Dominator);

      std::vector<llvm::Type*> args;
      for (Arg* arg : _section->entry()->args()) {
        args.push_back(emit_type(arg->type()));
      }

      _function = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(_context), args, false),
        llvm::Function::ExternalLinkage,
        name,
        module
      );

      section->autoname();
      _values.init(section);

      llvm::BasicBlock* entry_block = llvm::BasicBlock::Create(_context, "entry", _function);
      for (Block* block : *_section) {
        std::string name = "b" + std::to_string(block->name());
        _blocks[block] = llvm::BasicBlock::Create(_context, name, _function);
      }

      _builder.SetInsertPoint(entry_block);
      _builder.CreateBr(_blocks.at(section->entry()));

      for (Block* block : *_section) {
        _builder.SetInsertPoint(_blocks.at(block));
        for (Arg* arg : block->args()) {
          _values[arg] = emit_phi(arg);
        }
      }

      for (Arg* arg : _section->entry()->args()) {
        llvm::PHINode* phi = (llvm::PHINode*) _values.at(arg);
        phi->addIncoming(
          _function->getArg(arg->index()),
          entry_block
        );
      }

      for (Block* block : *_section) {
        _builder.SetInsertPoint(_blocks.at(block));
        for (Inst* inst : *block) {
          if (inst->is_terminator()) {
            _values[inst] = emit_terminator(inst, block);
          } else {
            _values[inst] = emit_inst(inst);
          }
        }
        _end_blocks[block] = _builder.GetInsertBlock();
      }
    }

    static void initilize_llvm_jit() {
      llvm::InitializeNativeTarget();
      llvm::InitializeNativeTargetAsmPrinter();
    }

    static void optimize_llvm(llvm::Module& module, llvm::OptimizationLevel level) {
      llvm::LoopAnalysisManager loop_analysis_manager;
      llvm::FunctionAnalysisManager function_analysis_manager;
      llvm::CGSCCAnalysisManager cgscc_analysis_manager;
      llvm::ModuleAnalysisManager module_analysis_manager;

      llvm::PassBuilder pass_builder;
      pass_builder.registerModuleAnalyses(module_analysis_manager);
      pass_builder.registerCGSCCAnalyses(cgscc_analysis_manager);
      pass_builder.registerFunctionAnalyses(function_analysis_manager);
      pass_builder.registerLoopAnalyses(loop_analysis_manager);
      pass_builder.crossRegisterProxies(
        loop_analysis_manager,
        function_analysis_manager,
        cgscc_analysis_manager,
        module_analysis_manager
      );

      llvm::ModulePassManager module_pass_manager = pass_builder.buildPerModuleDefaultPipeline(level);
      module_pass_manager.run(module, module_analysis_manager);
    }
  };
}
