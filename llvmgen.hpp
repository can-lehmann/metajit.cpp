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

#include "jitir.hpp"
#include "jitir_llvmapi.hpp"

namespace metajit {
  class LLVMCodeGen: public Pass<LLVMCodeGen> {
  private:
    bool _generating_extension = false;
    bool _comments = false; // Emit comments as references back to original IR

    Section* _section;
    llvm::LLVMContext& _context;
    llvm::Module* _module = nullptr;
    llvm::Function* _function = nullptr;
    llvm::IRBuilder<> _builder;
    LLVM_API _llvm_api; // Used for generating extension

    std::map<Block*, llvm::BasicBlock*> _blocks;
    std::map<Block*, llvm::BasicBlock*> _end_blocks;

    // Used for generating extension
    Uses _uses;
    ConstnessAnalysis _constness;
    TraceCapabilities _trace_capabilities;

    InstMap<llvm::Value*> _values;
    // Used for generating extension
    InstMap<llvm::Value*> _built;
    InstMap<llvm::Value*> _is_const;
    InstMap<llvm::Value*> _is_used;
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
      if (dynmatch(Const, constant, value)) {
        return llvm::ConstantInt::get(
          emit_type(constant->type()),
          constant->value(),
          false
        );
      } else if (dynmatch(Input, input, value)) {
        return _function->getArg(input->index());
      } else if (dynmatch(Inst, inst, value)) {
        return _values.at(inst);
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

    llvm::Value* emit_inst(Inst* inst) {
      if (dynmatch(FreezeInst, freeze, inst)) {
        return emit_arg(freeze->arg(0));
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
      } else if (dynmatch(CommentInst, comment, inst)) {
        return nullptr;
      }

      inst->write(std::cerr);
      std::cerr << std::endl;
      assert(false && "Unknown instruction");
      return nullptr;
    }

    llvm::Value* is_const(Value* value) {
      if (dynmatch(Const, constant, value)) {
        return llvm::ConstantInt::getTrue(_context);
      } else if (dynmatch(Input, input, value)) {
        return llvm::ConstantInt::getFalse(_context);
      } else if (dynmatch(Inst, inst, value)) {
        return _builder.CreateLoad(
          llvm::Type::getInt1Ty(_context),
          _is_const.at(inst)
        );
      } else {
        assert(false && "Unknown value");
        return nullptr;
      }
    }

    llvm::Value* emit_const_prop(Inst* inst) {
      if (dynamic_cast<FreezeInst*>(inst)) {
        return llvm::ConstantInt::getTrue(_context);
      } else if (dynamic_cast<AssumeConstInst*>(inst)) {
        return llvm::ConstantInt::getTrue(_context);
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
        return _builder.CreateOr(
          _builder.CreateAnd(
            cond_const,
            _builder.CreateSelect(cond, true_const, false_const) // Short-circuit
          ),
          _builder.CreateAnd(
            _builder.CreateAnd(
              true_const,
              false_const
            ),
            _builder.CreateICmpEQ(
              emit_arg(select->arg(1)),
              emit_arg(select->arg(2))
            )
          )
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
        assert(false);
        return nullptr;
      } else {
        llvm::Value* all_const = llvm::ConstantInt::getTrue(_context);
        for (Value* arg : inst->args()) {
          all_const = _builder.CreateAnd(all_const, is_const(arg));
        }
        return all_const;
      }
    }

    llvm::Value* is_used(Inst* inst) {
      return _builder.CreateLoad(
        llvm::Type::getInt1Ty(_context),
        _is_used.at(inst)
      );
    }

    llvm::Value* emit_used(Inst* inst) {
      if (inst->has_side_effect() || inst->is_terminator()) {
        return llvm::ConstantInt::getTrue(_context);
      } else {
        llvm::Value* used = llvm::ConstantInt::getFalse(_context);
        for (Uses::Use use : _uses.at(inst)) {
          llvm::Value* use_used = is_used(use.inst);
          
          if (is_int_or_bool(use.inst->type()) &&
              // Freeze is always constant, but needs this instruction to perform the check
              !dynamic_cast<FreezeInst*>(use.inst)) {
            use_used = _builder.CreateAnd(
              use_used,
              _builder.CreateNot(is_const(use.inst))
            );
          }

          if (dynmatch(SelectInst, select, use.inst)) {
            if (use.index > 0) {
              llvm::Value* branch_used = emit_arg(select->arg(0));
              if (branch_used) {
                if (use.index == 2) {
                  branch_used = _builder.CreateNot(branch_used);
                }
                use_used = _builder.CreateAnd(
                  use_used,
                  _builder.CreateSelect(
                    is_const(select->arg(0)),
                    branch_used,
                    llvm::ConstantInt::getTrue(_context)
                  )
                );
              }
            }
          }

          used = _builder.CreateOr(used, use_used);
        }
        return used;
      }
    }

    llvm::Value* emit_built_arg(Value* value) {
      if (dynmatch(Const, constant, value)) {
        llvm::Value* addr = llvm::ConstantInt::get(
          llvm::IntegerType::get(_context, sizeof(void*) * 8),
          (uint64_t)(void*) constant,
          false
        );

        return _builder.CreateIntToPtr(
          addr,
          llvm::PointerType::get(_context, 0)
        );
      } else if (dynmatch(Input, input, value)) {
        return _builder.CreateCall(
          _llvm_api.get_input,
          {
            _jitir_builder,
            llvm::ConstantInt::get(
              llvm::Type::getInt64Ty(_context),
              input->index()
            )
          }
        );
      } else if (dynmatch(Inst, inst, value)) {
        return _builder.CreateLoad(
          llvm::PointerType::get(_context, 0),
          _built.at(inst)
        );
      } else {
        assert(false && "Unknown value");
        return nullptr;
      }
    }

    // Codegen for the generating extension uses topological sorting.
    // Cycles are broken by selecting an overapproximation.
    // This is implemented using action groups where only one action of
    // each group needs to be executed. Actions within groups are prioritized
    // based on how much they overapproximate.
    // We need to ensure that it is always possible to select an action from
    // each group such that an acyclic execution order exists.

    struct ActionGroup;

    struct Action {
      std::string name;
      size_t priority = 0;
      std::function<void()> func;
      ActionGroup* group = nullptr;
      
      std::optional<size_t> executed;

      Action(const std::string& _name,
             size_t _priority,
             const std::function<void()>& _func,
             ActionGroup* _group):
        name(_name), priority(_priority), func(_func), group(_group) {}
    };

    struct ActionGroup {
      std::string name;
      std::vector<Action*> actions;
      
      // Actions that depend on this group
      std::vector<Action*> outgoing;
      std::vector<Action*> optional_outgoing;

      std::optional<size_t> executed;

      ActionGroup() {}
      ActionGroup(const std::string& _name): name(_name) {}

      ActionGroup(const ActionGroup&) = delete;
      ActionGroup& operator=(const ActionGroup&) = delete;

      ~ActionGroup() {
        for (Action* action : actions) {
          delete action;
        }
      }

      Action* add(const std::string& name,
                  size_t priority,
                  const std::vector<ActionGroup*>& dependencies,
                  const std::vector<ActionGroup*>& optional_dependencies,
                  std::function<void()> func) {
        
        Action* action = new Action(name, priority, func, this);
        actions.push_back(action);
        for (ActionGroup* group : dependencies) {
          if (group) {
            group->outgoing.push_back(action);
          }
        }
        for (ActionGroup* group : optional_dependencies) {
          if (group) {
            group->optional_outgoing.push_back(action);
          }
        }
        return action;
      }
    };

    class ActionQueue {
    private:
      std::vector<ActionGroup*> _groups;
    public:
      ActionQueue() {}
      ActionQueue(const ActionQueue&) = delete;
      ActionQueue& operator=(const ActionQueue&) = delete;

      ~ActionQueue() {
        for (ActionGroup* group : _groups) {
          delete group;
        }
      }

      ActionGroup* add(const std::string& name = "") {
        ActionGroup* group = new ActionGroup(name);
        _groups.push_back(group);
        return group;
      }
    
    private:
      void write_escaped(std::ostream& stream, const std::string& str) {
        for (char chr : str) {
          switch (chr) {
            case '\n': stream << "\\n"; break;
            case '\r': stream << "\\r"; break;
            case '\t': stream << "\\t"; break;
            case '"': stream << "\\\""; break;
            case '\\': stream << "\\\\"; break;
            case '{': stream << "\\{"; break;
            case '}': stream << "\\}"; break;
            case '<': stream << "\\<"; break;
            case '>': stream << "\\>"; break;
            case '|': stream << "\\|"; break;
            default:
              stream << chr;
          }
        }
      }
    public:
      void write_dot(std::ostream& stream) {
        stream << "digraph {\n";

        size_t id = 0;
        std::unordered_map<ActionGroup*, size_t> ids;
        std::unordered_map<Action*, size_t> action_indices;
        for (ActionGroup* group : _groups) {
          stream << "group" << id << " [shape=record, label=\"{";
          if (group->name.empty()) {
            stream << "(unnamed)";
          } else {
            write_escaped(stream, group->name);
          }
          for (size_t it = 0; it < group->actions.size(); it++) {
            stream << "|";
            Action* action = group->actions[it];
            stream << "<a" << it << "> ";
            std::string label = action->name;
            if (action->executed.has_value()) {
              label += "\n(executed at " + std::to_string(action->executed.value()) + ")";
            }
            write_escaped(stream, label);
            action_indices[action] = it;
          }
          stream << "}\"];\n";
          ids[group] = id;
          id++;
        }

        for (ActionGroup* group : _groups) {
          for (Action* action : group->outgoing) {
            stream << "group" << ids[group];
            stream << " -> group" << ids[action->group];
            //stream << ":a" << action_indices[action];
            stream << ";\n";
            if (action->executed.has_value() && group->executed.has_value()) {
              assert(action->executed.value() >= group->executed.value());
            }
          }

          for (Action* action : group->optional_outgoing) {
            stream << "group" << ids[group];
            stream << " -> group" << ids[action->group];
            //stream << ":a" << action_indices[action];
            stream << " [style=dashed";
            if (action->executed.has_value() && group->executed.has_value()) {
              if (action->executed.value() < group->executed.value()) {
                stream << ", color=red, weight=0, constraint=false";
              } else {
                stream << ", color=green";
              }
            }
            stream << "];\n";
          }
        }

        stream << "}\n";
      }

      void save_dot(const std::string& path) {
        std::ofstream file(path);
        assert(file.is_open());
        write_dot(file);
      }

      void run() {
        std::unordered_map<Action*, size_t> incoming;
        std::unordered_map<Action*, size_t> optional_incoming;
        std::unordered_map<Action*, size_t> fulfilled_optional;
        for (ActionGroup* group : _groups) {
          for (Action* action : group->outgoing) {
            incoming[action]++;
          }
          for (Action* action : group->optional_outgoing) {
            optional_incoming[action]++;
          }
        }

        struct Entry {
          Action* action;
          double optional_fraction;

          Entry(Action* _action,
                size_t fulfilled_optional,
                size_t optional_incoming):
              action(_action) {
            if (optional_incoming == 0) {
              optional_fraction = 1.0;
            } else {
              optional_fraction = (double)fulfilled_optional / (double)optional_incoming;
            }
          }
          
          bool operator<(const Entry& other) const {
            if ((optional_fraction == 1.0) != (other.optional_fraction == 1.0)) {
              return optional_fraction < other.optional_fraction;
            }
            return action->priority < other.action->priority;
          }
        };

        std::priority_queue<Entry> queue;

        #define push_action(action) \
          queue.push(Entry(action, fulfilled_optional[action], optional_incoming[action]));
        
        for (ActionGroup* group : _groups) {
          for (Action* action : group->actions) {
            if (incoming[action] == 0) {
              push_action(action);
            }
          }
        }

        size_t step = 0;
        while (!queue.empty()) {
          Action* action = queue.top().action;
          queue.pop();
          if (action->group->executed) {
            continue;
          }
          action->func();
          action->executed = step;
          action->group->executed = step;
          step++;

          for (Action* next : action->group->optional_outgoing) {
            if (!next->group->executed.has_value()) {
              fulfilled_optional[next]++;
              if (incoming[next] == 0) {
                push_action(next);
              } 
            }
          }

          for (Action* next : action->group->outgoing) {
            if (!next->group->executed.has_value()) {
              incoming[next]--;
              if (incoming[next] == 0) {
                push_action(next);
              }
            }
          }
        }

        #undef push_action

        for (ActionGroup* group : _groups) {
          assert(group->executed.has_value());
        }
      }
    };

    void emit_update_load_const(LoadInst* load) {
      llvm::Value* is_const_load = _builder.CreateCall(_llvm_api.is_const_inst, {emit_built_arg(load)});
      is_const_load = _builder.CreateTrunc(is_const_load, llvm::Type::getInt1Ty(_context));
      if (load->flags().has(LoadFlags::Pure)) {
        is_const_load = _builder.CreateOr(
          is_const_load,
          is_const(load->arg(0))
        );
      }
      _builder.CreateStore(is_const_load, _is_const.at(load));
    }

    

    void emit_build_inst(Inst* inst) {
      if (_comments &&
          !dynamic_cast<CommentInst*>(inst) &&
          !dynamic_cast<PhiInst*>(inst)) {
        std::ostringstream comment_stream;
        inst->write_arg(comment_stream);
        comment_stream << " = ";
        inst->write(comment_stream);
        _builder.CreateCall(_llvm_api.build_comment, {
          _jitir_builder,
          _builder.CreateGlobalString(comment_stream.str())
        });
      }

      emit_branch(
        is_used(inst),
        [&](){
          if (dynmatch(FreezeInst, freeze, inst)) {
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
                  _llvm_api.build_const_fast,
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
            return;
          } else if (dynmatch(AssumeConstInst, assume_const, inst)) {
            if (is_int_or_bool(assume_const->type())) {
              llvm::Value* built_const = _builder.CreateCall(
                _llvm_api.build_const_fast,
                {
                  _jitir_builder,
                  llvm::ConstantInt::get(llvm::Type::getInt32Ty(_context), (uint64_t)inst->type()),
                  _builder.CreateZExt(emit_arg(inst), llvm::Type::getInt64Ty(_context))
                }
              );

              _builder.CreateStore(
                built_const,
                _built.at(inst)
              );
            } else {
              _builder.CreateStore(
                emit_built_arg(assume_const->arg(0)),
                _built.at(inst)
              );
            }
            return;
          }

          std::vector<llvm::Value*> args;
          for (Value* arg : inst->args()) {
            args.push_back(emit_built_arg(arg));
          }

          if (is_int_or_bool(inst->type())) {
            auto trace_const = [&](){
              if (_trace_capabilities.can_trace_const(inst)) {
                llvm::Value* built = _builder.CreateCall(
                  _llvm_api.build_const_fast,
                  {
                    _jitir_builder,
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(_context), (uint64_t)inst->type()),
                    _builder.CreateZExt(emit_arg(inst), llvm::Type::getInt64Ty(_context))
                  }
                );
                _builder.CreateStore(built, _built.at(inst));
              }
            };
            if (_trace_capabilities.can_trace_const(inst) && !_trace_capabilities.can_trace_inst(inst)) {
              // This only happens for always const values, so we do not need to check is_const(inst)
              trace_const();
            } else {
              emit_branch(
                is_const(inst),
                trace_const,
                [&](){
                  if (_trace_capabilities.can_trace_inst(inst)) {
                    llvm::Value* built = build_build_inst(_builder, _llvm_api, inst, _jitir_builder, args);
                    _builder.CreateStore(built, _built.at(inst));

                    if (dynmatch(LoadInst, load, inst)) {
                      emit_update_load_const(load);
                    }
                  }
                },
                "const_"
              );
            }
          } else {
            if (_trace_capabilities.any(inst)) {
              llvm::Value* built = build_build_inst(_builder, _llvm_api, inst, _jitir_builder, args);
              _builder.CreateStore(built, _built.at(inst));

              if (dynmatch(LoadInst, load, inst)) {
                emit_update_load_const(load);
              }
            }
          }
        },
        [&](){},
        "used_"
      );
    }

    void emit_generating_extension(Block* block) {
      for (Inst* inst : *block) {
        if (dynmatch(PhiInst, phi, inst)) {
          for (size_t it = 0; it < phi->arg_count(); it++) {
            llvm::BasicBlock* from_block = _end_blocks.at(phi->block(it));
            llvm::Instruction* terminator = from_block->getTerminator();
            assert(terminator);
            terminator->removeFromParent();
            _builder.SetInsertPoint(from_block);
            _builder.CreateStore(is_const(phi->arg(it)), _is_const.at(inst));
            _builder.CreateStore(emit_built_arg(phi->arg(it)), _built.at(inst));
            _builder.Insert(terminator);
            _end_blocks.at(phi->block(it)) = _builder.GetInsertBlock();

            _builder.SetInsertPoint(_blocks.at(block));
            _values[inst] = emit_inst(inst);
          }
        } else {
          break;
        }
      }
      
      ActionQueue queue;

      std::unordered_map<Value*, ActionGroup*> emit_groups;
      std::unordered_map<Value*, ActionGroup*> const_groups;
      std::unordered_map<Value*, ActionGroup*> used_groups;
      ActionGroup* last_emit_group = nullptr;
      ActionGroup* last_build_group = nullptr;

      constexpr const size_t PRIO_MAX = 100;
      constexpr const size_t PRIO_OVERAPPROX_USED = 50;

      for (Inst* inst : *block) {
        if (dynamic_cast<PhiInst*>(inst) || inst->is_terminator()) {
          continue;
        }

        std::ostringstream name_stream;
        inst->write(name_stream);
        std::string name = name_stream.str();

        ActionGroup* emit_group = queue.add(name);
        ActionGroup* const_group = queue.add(name);
        ActionGroup* used_group = queue.add(name);
        ActionGroup* build_group = queue.add(name);

        std::vector<ActionGroup*> emit_deps;
        for (Value* arg : inst->args()) {
          if (emit_groups.find(arg) != emit_groups.end()) {
            emit_deps.push_back(emit_groups.at(arg));
          }
        }
        if (inst->has_side_effect() || dynamic_cast<LoadInst*>(inst)) {
          emit_deps.push_back(last_emit_group);
        }

        emit_group->add("emit", PRIO_MAX, emit_deps, {}, [inst, this](){
          _values[inst] = emit_inst(inst);  
        });

        std::vector<ActionGroup*> const_deps;
        if (!dynamic_cast<FreezeInst*>(inst) &&
            !dynamic_cast<AssumeConstInst*>(inst)) {
          const_deps.push_back(emit_group);
          for (Value* arg : inst->args()) {
            if (const_groups.find(arg) != const_groups.end()) {
              const_deps.push_back(const_groups.at(arg));
            }
          }
        }

        const_group->add("const", PRIO_MAX, const_deps, {}, [inst, this](){
          _builder.CreateStore(emit_const_prop(inst), _is_const.at(inst));
        });

        std::vector<ActionGroup*> build_deps = {const_group, used_group, last_build_group};
        build_group->add("build", PRIO_MAX, build_deps, {}, [inst, this](){
          emit_build_inst(inst);
        });

        emit_groups[inst] = emit_group;
        
        if (inst->has_side_effect() || dynamic_cast<LoadInst*>(inst)) {
          last_emit_group = emit_group;
        }

        if (dynmatch(LoadInst, load, inst)) {
          const_groups[inst] = build_group;
        } else {
          const_groups[inst] = const_group;
        }

        used_groups[inst] = used_group;

        last_build_group = build_group;
      }

      std::unordered_map<Inst*, bool> always_used;
      for (Inst* inst : block->rev_range()) {
        if (dynamic_cast<PhiInst*>(inst) || inst->is_terminator()) {
          continue;
        }

        always_used[inst] = false;
        if (inst->has_side_effect()) {
          always_used[inst] = true;
        } else {
          for (Uses::Use use : _uses.at(inst)) {
            if (always_used.find(use.inst) == always_used.end()) {
              always_used[inst] = true; // Used outside of current block or in terminator
            }
          }
        }
      }
            
      for (Inst* inst : *block) {
        if (dynamic_cast<PhiInst*>(inst) || inst->is_terminator()) {
          continue;
        }

        ActionGroup* used_group = used_groups[inst];

        if (always_used.at(inst)) {
          used_group->add("always used", PRIO_MAX, {}, {}, [inst, this](){
            _builder.CreateStore(
              llvm::ConstantInt::getTrue(_context),
              _is_used.at(inst)
            );
          });
        } else {
          std::vector<ActionGroup*> used_deps;

          for (Uses::Use use : _uses.at(inst)) {
            if (used_groups.find(use.inst) != used_groups.end()) {
              used_deps.push_back(used_groups.at(use.inst));
            }

            if (const_groups.find(use.inst) != const_groups.end()) {
              used_deps.push_back(const_groups.at(use.inst));
            }

            if (dynmatch(SelectInst, select, use.inst)) {
              if (use.index > 0 && emit_groups.find(select->cond()) != emit_groups.end()) {
                used_deps.push_back(emit_groups.at(select->cond()));
                used_deps.push_back(const_groups.at(select->cond()));
              }
            }
          }

          size_t prio = PRIO_MAX;
          if (dynmatch(LoadInst, load, inst)) {
            prio++;
          }
          used_group->add("used", prio, {}, used_deps, [inst, this](){
            _builder.CreateStore(
              emit_used(inst),
              _is_used.at(inst)
            );
          });
        }
      }

      _builder.SetInsertPoint(_blocks.at(block));
      queue.run();

      if (block->name() == 0) {
        queue.save_dot("action_queue.dot");
      }

      Inst* inst = block->terminator();
      assert(inst);
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
      } else {
        assert(false && "Unknown terminator");
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
        _llvm_api(module),
        _uses(section),
        _constness(section),
        _trace_capabilities(section, _constness) {
      
      std::vector<llvm::Type*> args;
      for (Input* input : _section->inputs()) {
        args.push_back(emit_type(input->type()));
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
      _is_const.init(section);
      _is_used.init(section);
      _built.init(section);

      llvm::BasicBlock* entry_block = llvm::BasicBlock::Create(_context, "entry", _function);
      for (Block* block : *_section) {
        _blocks[block] = llvm::BasicBlock::Create(_context, "block", _function);
      }

      _builder.SetInsertPoint(entry_block);

      if (_generating_extension) {
        _jitir_builder = _function->getArg(_section->inputs().size());

        for (Block* block : *section) {
          for (Inst* inst : *block) {
            _is_const[inst] = _builder.CreateAlloca(
              llvm::Type::getInt1Ty(_context),
              nullptr,
              "is_const"
            );

            // False is always a valid overapproximation
            _builder.CreateStore(
              llvm::ConstantInt::getFalse(_context),
              _is_const.at(inst)
            );

            _is_used[inst] = _builder.CreateAlloca(
              llvm::Type::getInt1Ty(_context),
              nullptr,
              "is_used"
            );

            // True is always a valid overapproximation 
            _builder.CreateStore(
              llvm::ConstantInt::getTrue(_context),
              _is_used.at(inst)
            );

            _built[inst] = _builder.CreateAlloca(
              llvm::PointerType::get(_context, 0),
              nullptr,
              "built"
            );

            _builder.CreateStore(
              llvm::ConstantPointerNull::get(llvm::PointerType::get(_context, 0)),
              _built.at(inst)
            );
          }
        }
      }

      _builder.CreateBr(_blocks.at(section->entry()));

      for (Block* block : *_section) {
        if (_generating_extension) {
          emit_generating_extension(block);
        } else {
          _builder.SetInsertPoint(_blocks.at(block));
          for (Inst* inst : *block) {
            _values[inst] = emit_inst(inst);
          }
        }
        
        _end_blocks[block] = _builder.GetInsertBlock();
      }
    }
  };
}
