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
    std::map<Block*, llvm::BasicBlock*> _end_blocks;

    ValueMap<llvm::Value*> _values;
    // Used for generating extension
    ValueMap<llvm::Value*> _built;
    ValueMap<llvm::Value*> _is_const;
    llvm::Value* _jitir_builder = nullptr;

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
        case Type::Ptr: return llvm::PointerType::get(_context, 0);
      }
      assert(false && "Unknown type");
      return nullptr;
    }

    llvm::Value* emit_arg(Value* value) {
      return _values.at(value);
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
          emit_add_offset(emit_arg(load->arg(0)), load->offset())
        );
      } else if (dynmatch(StoreInst, store, inst)) {
        return _builder.CreateStore(
          emit_arg(store->arg(1)),
          emit_add_offset(emit_arg(store->arg(0)), store->offset())
        );
      } else if (dynmatch(AddPtrInst, add_ptr, inst)) {
        return _builder.CreateGEP(
          llvm::Type::getInt8Ty(_context),
          emit_arg(add_ptr->arg(0)),
          {emit_arg(add_ptr->arg(1))}
        );
      } else if (dynmatch(AddPtrConstInst, add_ptr_const, inst)) {
        return emit_add_offset(
          emit_arg(add_ptr_const->arg(0)),
          add_ptr_const->offset()
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
      binop(ModS, SRem)
      binop(ModU, URem)
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
      } else if (dynmatch(PhiInst, phi, inst)) {
        llvm::PHINode* phi_node = _builder.CreatePHI(
          emit_type(phi->type()),
          phi->arg_count()
        );
        for (size_t it = 0; it < phi->arg_count(); it++) {
          phi_node->addIncoming(
            emit_arg(phi->arg(it)),
            _end_blocks.at(phi->block(it))
          );
        }
        return phi_node;
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
      } else if (dynmatch(PhiInst, phi, inst)) {
        llvm::PHINode* phi_node = _builder.CreatePHI(
          llvm::Type::getInt1Ty(_context),
          phi->arg_count()
        );
        for (size_t it = 0; it < phi->arg_count(); it++) {
          phi_node->addIncoming(
            is_const(phi->arg(it)),
            _end_blocks.at(phi->block(it))
          );
        }
        return phi_node;
      } else {
        llvm::Value* all_const = llvm::ConstantInt::getTrue(_context);
        for (Value* arg : inst->args()) {
          all_const = _builder.CreateAnd(all_const, is_const(arg));
        }
        return all_const;
      }
    }

    llvm::Value* emit_built_arg(Value* value) {
      return _builder.CreateLoad(
        llvm::PointerType::get(_context, 0),
        _built.at(value)
      );
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

      llvm::BasicBlock* entry_block = llvm::BasicBlock::Create(_context, "entry", _function);
      for (Block* block : *_section) {
        _blocks[block] = llvm::BasicBlock::Create(_context, "block", _function);
      }

      _builder.SetInsertPoint(entry_block);

      if (_generating_extension) {
        _jitir_builder = _function->getArg(_section->inputs().size() + _section->outputs().size());

        for (Block* block : *section) {
          for (Inst* inst : *block) {
            _built[inst] = _builder.CreateAlloca(
              llvm::PointerType::get(_context, 0),
              nullptr,
              "built"
            );
          }
        }
      }

      _builder.CreateBr(_blocks.at(section->entry()));

      for (Block* block : *_section) {
        if (_generating_extension) {
          for (Inst* inst : *block) {
            if (dynmatch(PhiInst, phi, inst)) {
              for (size_t it = 0; it < phi->arg_count(); it++) {
                llvm::BasicBlock* from_block = _end_blocks.at(phi->block(it));
                llvm::Instruction* terminator = from_block->getTerminator();
                assert(terminator);
                terminator->removeFromParent();
                _builder.SetInsertPoint(from_block);
                _builder.CreateStore(emit_built_arg(phi->arg(it)), _built.at(inst));
                _builder.Insert(terminator);
                _end_blocks.at(phi->block(it)) = _builder.GetInsertBlock();
              }
            } else {
              break;
            }
          }
        }

        _builder.SetInsertPoint(_blocks.at(block));
        for (Inst* inst : *block) {
          if (_generating_extension) {
            if (dynmatch(BranchInst, branch, inst)) {
              _builder.CreateCall(_llvm_api.build_guard, {
                _jitir_builder,
                emit_built_arg(branch->arg(0)),
                _builder.CreateZExt(
                  emit_arg(branch->arg(0)),
                  llvm::Type::getInt32Ty(_context)
                )
              });
              _values[inst] = emit_inst(inst);
            } else if (dynamic_cast<JumpInst*>(inst) ||
                       dynamic_cast<ExitInst*>(inst)) {
              _values[inst] = emit_inst(inst);
            } else if (dynmatch(PhiInst, phi, inst)) {
              _values[inst] = emit_inst(inst);
              _is_const[inst] = emit_const_prop(inst);

            } else if (dynmatch(FreezeInst, freeze, inst)) {
              _values[inst] = emit_inst(inst);
              _is_const[inst] = llvm::ConstantInt::getTrue(_context);

              emit_branch(
                is_const(freeze->arg(0)),
                [&](){
                  _builder.CreateStore(
                    emit_built_arg(freeze->arg(0)),
                    _built.at(inst)
                  );
                },
                [&](){
                  llvm::Value* built_const = _builder.CreateCall(
                    _llvm_api.build_const,
                    {
                      _jitir_builder,
                      llvm::ConstantInt::get(llvm::Type::getInt32Ty(_context), (uint64_t)inst->type()),
                      _builder.CreateZExt(emit_arg(inst), llvm::Type::getInt64Ty(_context))
                    }
                  );

                  _builder.CreateCall(_llvm_api.build_guard, {
                    _jitir_builder,
                    _builder.CreateCall(
                      _llvm_api.build_eq,
                      {
                        _jitir_builder,
                        emit_built_arg(freeze->arg(0)),
                        built_const
                      }
                    ),
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(_context), 1)
                  });

                  _builder.CreateStore(
                    built_const,
                    _built.at(inst)
                  );
                },
                "freeze_"
              );
            } else {
              _values[inst] = emit_inst(inst);
              _is_const[inst] = emit_const_prop(inst);
              
              std::vector<llvm::Value*> args;
              for (Value* arg : inst->args()) {
                args.push_back(emit_built_arg(arg));
              }

              auto emit_else = [&](){
                llvm::Value* built = build_build_inst(_builder, _llvm_api, inst, _jitir_builder, args);
                _builder.CreateStore(built, _built.at(inst));
              };

              if (is_int_or_bool(inst->type()) && !dynamic_cast<ConstInst*>(inst) && !is_never_const(inst)) {
                emit_branch(
                  is_const(inst),
                  [&](){
                    llvm::Value* built = _builder.CreateCall(
                      _llvm_api.build_const,
                      {
                        _jitir_builder,
                        llvm::ConstantInt::get(llvm::Type::getInt32Ty(_context), (uint64_t)inst->type()),
                        _builder.CreateZExt(emit_arg(inst), llvm::Type::getInt64Ty(_context))
                      }
                    );
                    _builder.CreateStore(built, _built.at(inst));
                  },
                  emit_else,
                  "const_"
                );
              } else {
                emit_else();
              }

              if (dynamic_cast<LoadInst*>(inst)) {
                llvm::Value* is_const_inst = _builder.CreateCall(_llvm_api.is_const_inst, {emit_built_arg(inst)});
                _is_const[inst] = _builder.CreateTrunc(is_const_inst, llvm::Type::getInt1Ty(_context));
              }
            }
          } else {
            _values[inst] = emit_inst(inst);
          }
        }
        
        _end_blocks[block] = _builder.GetInsertBlock();
      }
    }
  };
}
