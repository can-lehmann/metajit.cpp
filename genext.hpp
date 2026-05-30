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

#include "jitir.hpp"

namespace metajit {
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

  class CreateGenExt: public Pass<CreateGenExt> {
  private:
    Section* _section;
    Section* _genext_section;

    Builder _builder;

    Uses _uses;
    BindingTimeGroups _binding_time_groups;
    TraceCapabilities _trace_capabilities;

    NameMap<Value*> _is_const;
    NameMap<Value*> _is_used;
    NameMap<Value*> _built;
    Value* _jitir_builder = nullptr;

    Value* is_const(Value* value) {
      if (dynmatch(Const, constant, value)) {
        return _builder.build_const(Type::Bool, 1);
      } else if (value->is_named()) {
        return _builder.build_load(
          _is_const.at((NamedValue*) value),
          Type::Bool,
          LoadFlags::None,
          AliasingGroup(0),
          0
        );
      } else {
        assert(false && "Unknown value");
        return nullptr;
      }
    }

    Value* emit_const_prop(Inst* inst) {
      // all the cases that use the instruction args and call emit_arg need to
      // emit an LLVM Freeze instruction to deal with the possibility of the
      // argument being a poison value. the result of this method must not be
      // poison.
      if (dynamic_cast<PromoteInst*>(inst)) {
        return _builder.build_const(Type::Bool, 1);
      } else if (dynamic_cast<AssumeConstInst*>(inst)) {
        return _builder.build_const(Type::Bool, 1);
      } else if (dynmatch(LoadInst, load, inst)) {
        if (load->flags().has(LoadFlags::Pure)) {
          return is_const(load->arg(0));
        } else {
          return _builder.build_const(Type::Bool, 0);
        }
      } else if (dynmatch(SelectInst, select, inst)) {
        Value* cond_const = is_const(select->arg(0));
        Value* true_const = is_const(select->arg(1));
        Value* false_const = is_const(select->arg(2));
        Value* cond = _builder.build_freeze(emit_arg(select->arg(0)));
        return _builder.fold_or(
          _builder.fold_and(
            cond_const,
            _builder.fold_select(cond, true_const, false_const) // Short-circuit
          ),
          _builder.fold_and(
            _builder.fold_and(
              true_const,
              false_const
            ),
            _builder.fold_eq(
              _builder.build_freeze(emit_arg(select->arg(1))),
              _builder.build_freeze(emit_arg(select->arg(2)))
            )
          )
        );
      } else if (dynmatch(AndInst, and_inst, inst)) {
        Value* a_const = is_const(and_inst->arg(0));
        Value* b_const = is_const(and_inst->arg(1));
        Value* a = _builder.build_freeze(emit_arg(and_inst->arg(0)));
        Value* b = _builder.build_freeze(emit_arg(and_inst->arg(1)));

        Value* res = _builder.fold_and(a_const, b_const);
        // Short-circuit
        res = _builder.fold_or(res, _builder.fold_and(a_const,
          _builder.fold_eq(a, _builder.build_const(a->type(), 0))
        ));
        res = _builder.fold_or(res, _builder.fold_and(b_const,
          _builder.fold_eq(b, _builder.build_const(b->type(), 0))
        ));

        return res;
      } else if (dynmatch(OrInst, or_inst, inst)) {
        Value* a_const = is_const(or_inst->arg(0));
        Value* b_const = is_const(or_inst->arg(1));
        Value* a = _builder.build_freeze(emit_arg(or_inst->arg(0)));
        Value* b = _builder.build_freeze(emit_arg(or_inst->arg(1)));

        Value* res = _builder.fold_and(a_const, b_const);
        // Short-circuit
        res = _builder.fold_or(res, _builder.fold_and(a_const,
          _builder.fold_eq(a, _builder.build_const(a->type(), type_mask(a->type())))
        ));
        res = _builder.fold_or(res, _builder.fold_and(b_const,
          _builder.fold_eq(b, _builder.build_const(b->type(), type_mask(b->type())))
        ));

        return res;
      } else {
        if (inst->has_side_effect() ||
            inst->is_terminator() ||
            dynamic_cast<CommentInst*>(inst)) {
          return _builder.build_const(Type::Bool, 0);
        }

        Value* all_const = _builder.build_const(Type::Bool, 1);
        for (Value* arg : inst->args()) {
          all_const = _builder.fold_and(all_const, is_const(arg));
        }
        return all_const;
      }
    }

    Value* is_used(Inst* inst) {
      return _builder.build_load(
        _is_used.at(inst),
        Type::Bool,
        LoadFlags::None,
        AliasingGroup(0),
        0
      );
    }

    Value* emit_used(Inst* inst) {
      if (inst->has_side_effect() || inst->is_terminator()) {
        return _builder.build_const(Type::Bool, 1);
      } else {
        Value* used = _builder.build_const(Type::Bool, 0);
        for (Uses::Use use : _uses.at(inst)) {
          Value* use_used = is_used(use.inst);
          
          if (is_int_or_bool(use.inst->type()) &&
              // Promote is always constant, but needs this instruction to perform the check
              !dynamic_cast<PromoteInst*>(use.inst)) {
            use_used = _builder.fold_and(
              use_used,
              _builder.fold_not(is_const(use.inst))
            );
          }

          if (dynmatch(SelectInst, select, use.inst)) {
            if (use.index > 0) {
              Value* branch_used = _builder.build_freeze(emit_arg(select->arg(0)));
              if (branch_used) {
                if (use.index == 2) {
                  branch_used = _builder.fold_not(branch_used);
                }
                use_used = _builder.fold_and(
                  use_used,
                  _builder.fold_select(
                    is_const(select->arg(0)),
                    branch_used,
                    _builder.build_const(Type::Bool, 1)
                  )
                );
              }
            }
          }

          used = _builder.fold_or(used, use_used);
        }
        return used;
      }
    }

    Value* emit_built_arg(Value* value) {
      if (dynmatch(Const, constant, value)) {
        return _builder.build_const(
          Type::Ptr, (uint64_t)(void*) constant
        );
      } else if (value->is_named()) {
        return _builder.build_load(
          _built.at((NamedValue*) value),
          Type::Ptr,
          LoadFlags::None,
          AliasingGroup(0),
          0
        );
      } else {
        assert(false && "Unknown value");
        return nullptr;
      }
    }

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
          !dynamic_cast<CommentInst*>(inst)) {
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
          if (dynmatch(PromoteInst, promote, inst)) {
            if (is_int_or_bool(promote->type())) {
              emit_branch(
                is_const(promote->arg(0)),
                [&](){
                  _builder.CreateStore(
                    emit_built_arg(promote->arg(0)),
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
                        emit_built_arg(promote->arg(0)),
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
              _builder.CreateStore(
                emit_built_arg(promote->arg(0)),
                _built.at(inst)
              );
            }
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
      ActionQueue queue;

      std::unordered_map<Value*, ActionGroup*> emit_groups;
      std::unordered_map<Value*, ActionGroup*> const_groups;
      std::unordered_map<Value*, ActionGroup*> used_groups;
      ActionGroup* last_emit_group = nullptr;
      ActionGroup* last_build_group = nullptr;

      constexpr const size_t PRIO_MAX = 100;
      constexpr const size_t PRIO_OVERAPPROX_USED = 50;

      for (Inst* inst : *block) {
        if (inst->is_terminator()) {
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
        if (!dynamic_cast<PromoteInst*>(inst) &&
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
        if (dynamic_cast<PromoteInst*>(inst)) {
          build_deps.push_back(emit_group);
          if (const_groups.find(inst->arg(0)) != const_groups.end()) {
            build_deps.push_back(const_groups.at(inst->arg(0)));
          }
        }
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
        if (inst->is_terminator()) {
          continue;
        }

        always_used[inst] = false;
        if (inst->has_side_effect() || dynamic_cast<CommentInst*>(inst)) {
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
        if (inst->is_terminator()) {
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
      } else if (dynmatch(JumpInst, jump, inst)) {
        for (Arg* arg : jump->block()->args()) {
          _builder.CreateStore(is_const(jump->arg(arg->index())), _is_const.at(arg));
          _builder.CreateStore(emit_built_arg(jump->arg(arg->index())), _built.at(arg));
        }
      }
      _values[inst] = emit_terminator(inst, block);
    }

    void emit_generating_extension_allocs(NamedValue* value) {
      _is_const[value] = _builder.CreateAlloca(
        llvm::Type::getInt1Ty(_context),
        nullptr,
        "is_const"
      );

      // False is always a valid overapproximation
      _builder.CreateStore(
        llvm::ConstantInt::getFalse(_context),
        _is_const.at(value)
      );

      _is_used[value] = _builder.CreateAlloca(
        llvm::Type::getInt1Ty(_context),
        nullptr,
        "is_used"
      );

      // True is always a valid overapproximation 
      _builder.CreateStore(
        llvm::ConstantInt::getTrue(_context),
        _is_used.at(value)
      );

      _built[value] = _builder.CreateAlloca(
        llvm::PointerType::get(_context, 0),
        nullptr,
        "built"
      );

      _builder.CreateStore(
        llvm::ConstantPointerNull::get(llvm::PointerType::get(_context, 0)),
        _built.at(value)
      );
    }

    void emit_entry() {
      _jitir_builder = _function->getArg(_section->entry()->args().size());

      for (Block* block : *section) {
        for (Arg* arg : block->args()) {
          emit_generating_extension_allocs(arg);
        }

        for (Inst* inst : *block) {
          emit_generating_extension_allocs(inst);
        }
      }

      for (Arg* arg : _section->entry()->args()) {
        _builder.CreateStore(
          _builder.CreateCall(
            _llvm_api.entry_arg,
            {
              _jitir_builder,
              llvm::ConstantInt::get(
                llvm::Type::getInt64Ty(_context),
                arg->index()
              )
            }
          ),
          _built.at(arg)
        );
      }
    }
  public:
    CreateGenExt(Section* section, Section* genext_section):
        Pass(section),
        _section(section),
        _genext_section(genext_section),
        _builder(genext_section),
        _uses(section),
        _binding_time_groups(section),
        _trace_capabilities(section, _binding_time_groups) {
      
      section->autoname();
      _is_const.init(section);
      _is_used.init(section);
      _built.init(section);

      emit_entry();
    }
  };
}
