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

}
