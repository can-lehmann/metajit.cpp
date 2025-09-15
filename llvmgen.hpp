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

#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/IRBuilder.h"

#include "jitir.hpp"
#include "jitir_llvmapi.hpp"

namespace metajit {
  class LLVMCodeGen: public Pass<LLVMCodeGen> {
  private:
    bool _generating_extension = false;
    Section* _section;
    llvm::LLVMContext& _context;
    llvm::Module* _module = nullptr;
    llvm::Function* _function = nullptr;
    llvm::IRBuilder<> _builder;
    LLVM_API _llvm_api; // Used for generating extension

    std::map<Block*, llvm::BasicBlock*> _blocks;
    ValueMap<llvm::Value*> _values;
    // Used for generating extension
    ValueMap<llvm::Value*> _built;
    ValueMap<llvm::Value*> _is_const;

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
      } else if (dynmatch(FreezeInst, freeze, inst)) {
        return emit_arg(freeze->arg(0));
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
      } else if (dynmatch(ResizeUInst, resize_u, inst)) {
        return _builder.CreateZExtOrTrunc(
          emit_arg(resize_u->arg(0)),
          emit_type(resize_u->type())
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

      else if (dynmatch(BranchInst, branch, inst)) {
        return _builder.CreateCondBr(
          emit_arg(branch->cond()),
          _blocks.at(branch->true_block()),
          _blocks.at(branch->false_block())
        );
      } else if (dynmatch(JumpInst, jump, inst)) {
        return _builder.CreateBr(_blocks.at(jump->block()));
      } else if (dynmatch(ExitInst, exit, inst)) {
        return _builder.CreateRetVoid();
      }

      inst->write(std::cerr);
      std::cerr << std::endl;
      assert(false && "Unknown instruction");
    }

    llvm::Value* is_const(Value* value) {
      return _is_const.at(value);
    }

    bool is_never_const(Value* value) {
      if (llvm::ConstantInt* constant_int = llvm::dyn_cast<llvm::ConstantInt>(is_const(value))) {
        return constant_int->isZero();
      }
      return false;
    }

    llvm::Value* emit_const_prop(Inst* inst) {
      if (dynamic_cast<ConstInst*>(inst) || dynamic_cast<FreezeInst*>(inst)) {
        return llvm::ConstantInt::getTrue(_context);
      } else if (dynmatch(InputInst, input, inst)) {
        if (input->flags().has(InputFlags::AssumeConst)) {
          return llvm::ConstantInt::getTrue(_context);
        } else {
          return llvm::ConstantInt::getFalse(_context);
        }
      } else if (dynmatch(LoadInst, load, inst)) {
        if (load->flags().has(LoadFlags::Pure)) {
          return is_const(load->arg(0));
        } else {
          return llvm::ConstantInt::getFalse(_context);
        }
      } else if (dynmatch(SelectInst, select, inst)) {
        llvm::Value* cond_const = is_const(select->arg(0));
        llvm::Value* true_const = is_const(select->arg(1));
        llvm::Value* false_const = is_const(select->arg(2));
        llvm::Value* cond = emit_arg(select->arg(0));
        return _builder.CreateSelect(cond_const,
          _builder.CreateSelect(cond, true_const, false_const), // Short-circuit
          _builder.CreateAnd(true_const, false_const)
        );
      } else if (dynmatch(AndInst, and_inst, inst)) {
        llvm::Value* a_const = is_const(and_inst->arg(0));
        llvm::Value* b_const = is_const(and_inst->arg(1));
        llvm::Value* a = emit_arg(and_inst->arg(0));
        llvm::Value* b = emit_arg(and_inst->arg(1));

        llvm::APInt zero = llvm::APInt::getZero(a->getType()->getIntegerBitWidth());

        llvm::Value* res = _builder.CreateAnd(a_const, b_const);
        // Short-circuit
        res = _builder.CreateOr(res, _builder.CreateAnd(a_const,
          _builder.CreateICmpEQ(a, llvm::ConstantInt::get(a->getType(), zero))
        ));
        res = _builder.CreateOr(res, _builder.CreateAnd(b_const,
          _builder.CreateICmpEQ(b, llvm::ConstantInt::get(b->getType(), zero))
        ));

        return res;
      } else if (dynmatch(OrInst, or_inst, inst)) {
        llvm::Value* a_const = is_const(or_inst->arg(0));
        llvm::Value* b_const = is_const(or_inst->arg(1));
        llvm::Value* a = emit_arg(or_inst->arg(0));
        llvm::Value* b = emit_arg(or_inst->arg(1));

        llvm::APInt ones = llvm::APInt::getMaxValue(a->getType()->getIntegerBitWidth());

        llvm::Value* res = _builder.CreateAnd(a_const, b_const);
        // Short-circuit
        res = _builder.CreateOr(res, _builder.CreateAnd(a_const,
          _builder.CreateICmpEQ(a, llvm::ConstantInt::get(a->getType(), ones))
        ));
        res = _builder.CreateOr(res, _builder.CreateAnd(b_const,
          _builder.CreateICmpEQ(b, llvm::ConstantInt::get(b->getType(), ones))
        ));

        return res;
      } else {
        llvm::Value* all_const = llvm::ConstantInt::getTrue(_context);
        for (Value* arg : inst->args()) {
          all_const = _builder.CreateAnd(all_const, is_const(arg));
        }
        return all_const;
      }
    }

  public:
    LLVMCodeGen(Section* section,
                llvm::Module* module,
                const std::string& name,
                bool generating_extension = false):
        Pass(section),
        _generating_extension(generating_extension),
        _section(section),
        _context(module->getContext()),
        _module(module),
        _builder(module->getContext()),
        _llvm_api(module) {
      
      std::vector<llvm::Type*> args;
      for (Type type : _section->inputs()) {
        args.push_back(emit_type(type));
      }

      for (Type type : _section->outputs()) {
        args.push_back(llvm::PointerType::get(_context, 0));
      }

      if (_generating_extension) {
        args.push_back(llvm::PointerType::get(_context, 0));
      }

      _function = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(_context), args, false),
        llvm::Function::ExternalLinkage,
        name,
        module
      );

      section->autoname();
      _values.init(section);
      _built.init(section);
      _is_const.init(section);

      for (Block* block : *_section) {
        _blocks[block] = llvm::BasicBlock::Create(_context, "block", _function);
      }

      for (Block* block : *_section) {
        _builder.SetInsertPoint(_blocks.at(block));

        for (Inst* inst : *block) {
          if (_generating_extension) {
            llvm::Value* jitir_builder = _function->getArg(_section->inputs().size() + _section->outputs().size());
            if (dynmatch(BranchInst, branch, inst)) {
              _builder.CreateCall(_llvm_api.build_guard, {
                jitir_builder,
                _built.at(branch->arg(0)),
                _builder.CreateZExt(emit_arg(branch->arg(0)), llvm::Type::getInt32Ty(_context))
              });
              _values[inst] = emit_inst(inst);
            } else if (dynmatch(FreezeInst, freeze, inst)) {
              _values[inst] = emit_inst(inst);
              _is_const[inst] = llvm::ConstantInt::getTrue(_context);

              llvm::BasicBlock* when_const = llvm::BasicBlock::Create(_context, "when_const", _function);
              llvm::BasicBlock* when_not_const = llvm::BasicBlock::Create(_context, "when_not_const", _function);
              llvm::BasicBlock* cont = llvm::BasicBlock::Create(_context, "cont", _function);

              _builder.CreateCondBr(is_const(freeze->arg(0)), when_const, when_not_const);

              _builder.SetInsertPoint(when_const);
              _builder.CreateBr(cont);

              _builder.SetInsertPoint(when_not_const);

              llvm::Value* built_const = _builder.CreateCall(
                _llvm_api.build_const,
                {
                  jitir_builder,
                  llvm::ConstantInt::get(llvm::Type::getInt32Ty(_context), (uint64_t)inst->type()),
                  _builder.CreateZExt(emit_arg(inst), llvm::Type::getInt64Ty(_context))
                }
              );

              _builder.CreateCall(_llvm_api.build_guard, {
                jitir_builder,
                _builder.CreateCall(
                  _llvm_api.build_eq,
                  {
                    jitir_builder,
                    _built.at(freeze->arg(0)),
                    built_const
                  }
                ),
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(_context), 1)
              });

              _builder.CreateBr(cont);

              _builder.SetInsertPoint(cont);
              llvm::PHINode* phi = _builder.CreatePHI(llvm::PointerType::get(_context, 0), 2);
              phi->addIncoming(_built.at(freeze->arg(0)), when_const);
              phi->addIncoming(built_const, when_not_const);
              _built[inst] = phi;
            } else if (!dynamic_cast<ExitInst*>(inst) && !dynamic_cast<JumpInst*>(inst)) {
              _values[inst] = emit_inst(inst);
              _is_const[inst] = emit_const_prop(inst);
              
              std::vector<llvm::Value*> args;
              for (Value* arg : inst->args()) {
                args.push_back(_built.at(arg));
              }

              if (is_int_or_bool(inst->type()) && !dynamic_cast<ConstInst*>(inst) && !is_never_const(inst)) {
                llvm::BasicBlock* when_const = llvm::BasicBlock::Create(_context, "when_const", _function);
                llvm::BasicBlock* when_not_const = llvm::BasicBlock::Create(_context, "when_not_const", _function);
                llvm::BasicBlock* cont = llvm::BasicBlock::Create(_context, "cont", _function);

                _builder.CreateCondBr(is_const(inst), when_const, when_not_const);

                _builder.SetInsertPoint(when_const);
                llvm::Value* built_const = _builder.CreateCall(
                  _llvm_api.build_const,
                  {
                    jitir_builder,
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(_context), (uint64_t)inst->type()),
                    _builder.CreateZExt(emit_arg(inst), llvm::Type::getInt64Ty(_context))
                  }
                );
                _builder.CreateBr(cont);

                _builder.SetInsertPoint(when_not_const);
                llvm::Value* built_inst = build_build_inst(_builder, _llvm_api, inst, jitir_builder, args);
                _builder.CreateBr(cont);

                _builder.SetInsertPoint(cont);
                llvm::PHINode* phi = _builder.CreatePHI(llvm::PointerType::get(_context, 0), 2);
                phi->addIncoming(built_const, when_const);
                phi->addIncoming(built_inst, when_not_const);
                _built[inst] = phi;
              } else {
                _built[inst] = build_build_inst(_builder, _llvm_api, inst, jitir_builder, args);
              }

              if (dynamic_cast<LoadInst*>(inst)) {
                llvm::Value* is_const_inst = _builder.CreateCall(_llvm_api.is_const_inst, {_built.at(inst)});
                _is_const[inst] = _builder.CreateTrunc(is_const_inst, llvm::Type::getInt1Ty(_context));
              }
            } else {
              _values[inst] = emit_inst(inst);
            }
          } else {
            _values[inst] = emit_inst(inst);
          }
        } 
      }
    }
  };
}
