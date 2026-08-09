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

#include <new>
#include <map>
#include <vector>
#include <stdexcept>

#include "jitir.hpp"

namespace metajit {
  class Layout2Aliasing: public Pass<Layout2Aliasing> {
  public:
    class Layout {
    private:
      bool _singleton = false;
    public:
      Layout(bool singleton): _singleton(singleton) {}
      virtual ~Layout() = default;

      bool singleton() const { return _singleton; }
    };

    class Record: public Layout {
    private:
      std::map<uint64_t, Layout*> _fields;
    public:
      Record(bool singleton, std::map<uint64_t, Layout*> fields):
        Layout(singleton), _fields(std::move(fields)) {}

      Layout* field_at(uint64_t offset) const {
        auto it = _fields.find(offset);
        if (it == _fields.end()) {
          return nullptr;
        }
        return it->second;
      }
    };

    class Array: public Layout {
    private:
      Layout* _element;
    public:
      Array(bool singleton, Layout* element):
        Layout(singleton), _element(element) {}

      Layout* element() const { return _element; }
    };

    class Heap: public Layout {
    public:
      Heap(): Layout(false) {}
    };

    class LayoutBuilder {
    private:
      Allocator* _allocator = nullptr;
      bool _owns_allocator = false;
    public:
      LayoutBuilder(): _allocator(new ArenaAllocator()), _owns_allocator(true) {}
      LayoutBuilder(Allocator& allocator): _allocator(&allocator), _owns_allocator(false) {}

      ~LayoutBuilder() {
        if (_owns_allocator && _allocator) {
          delete _allocator;
        }
      }

      Heap* heap() {
        return new (_allocator->alloc<Heap>()) Heap();
      }

      Array* array(bool singleton, Layout* element) {
        return new (_allocator->alloc<Array>()) Array(singleton, element);
      }

      Record* record(bool singleton, std::map<uint64_t, Layout*> fields) {
        return new (_allocator->alloc<Record>()) Record(singleton, std::move(fields));
      }

      Array* array(Layout* element) { return array(false, element); }
      Array* array_singleton(Layout* element) { return array(true, element); }
      Record* record(std::map<uint64_t, Layout*> fields) { return record(false, std::move(fields)); }
      Record* record_singleton(std::map<uint64_t, Layout*> fields) { return record(true, std::move(fields)); }
    };

  private:
    NameMap<Layout*> _layouts;
    std::map<std::pair<const Layout*, uint64_t>, AliasingGroup> _group_ids;
    AliasingGroup _next_group = 1;
    AliasingGroup _next_exact_group = -1;
    inline static Heap _call_result_heap;

    AliasingGroup group_for(Layout* node, uint64_t offset) {
      if (dynamic_cast<Heap*>(node)) {
        return 0;
      }
      if (dynamic_cast<Array*>(node)) {
        offset = 0;
      }
      auto key = std::make_pair(node, offset);
      if (_group_ids.find(key) == _group_ids.end()) {
        if (dynamic_cast<Record*>(node) && node->singleton()) {
          _group_ids[key] = _next_exact_group--;
        } else {
          _group_ids[key] = _next_group++;
        }
      }
      return _group_ids[key];
    }

    Layout* at(Value* value) const {
      if (value->is_named()) {
        return _layouts[(NamedValue*) value];
      } else {
        return nullptr;
      }
    }

    bool meet(NamedValue* target, Layout* incoming) {
      if (!incoming) {
        return false;
      }
      Layout*& current = _layouts[target];
      if (!current) {
        current = incoming;
        return true;
      }
      if (current == incoming || (dynamic_cast<Heap*>(current) && dynamic_cast<Heap*>(incoming))) {
        return false;
      }
      throw std::runtime_error("Layout2Aliasing: conflicting layouts merge at %" + std::to_string(target->name()));
    }

    Layout* child_of(Layout* node, uint64_t offset) {
      if (dynmatch(Record, record, node)) {
        Layout* field = record->field_at(offset);
        if (field) {
          return field;
        }
        throw std::runtime_error("Layout2Aliasing: no declared field at offset " + std::to_string(offset));
      } else if (dynmatch(Array, array, node)) {
        return array->element();
      } else if (dynamic_cast<Heap*>(node)) {
        return node;
      }
      throw std::runtime_error("Layout2Aliasing: unknown layout node kind");
    }

    void find_layouts(Section* section) {
      bool changed = true;
      while (changed) {
        changed = false;
        for (Block* block : *section) {
          for (Inst* inst : *block) {
            if (dynmatch(AddPtrInst, add_ptr, inst)) {
              Layout* ptr = at(add_ptr->ptr());
              if (dynamic_cast<Record*>(ptr)) {
                throw std::runtime_error("Layout2Aliasing: AddPtr into Record layout is not supported at %" + std::to_string(add_ptr->name()));
              }
              changed |= meet(add_ptr, ptr);
            } else if (dynmatch(LoadInst, load, inst)) {
              Layout* ptr = at(load->ptr());
              if (ptr && load->type() == Type::Ptr) {
                changed |= meet(load, child_of(ptr, load->offset()));
              }
            } else if (dynmatch(JumpInst, jump, inst)) {
              for (Arg* arg : jump->block()->args()) {
                changed |= meet(arg, at(jump->arg(arg->index())));
              }
            } else if (dynmatch(CallInst, call, inst)) {
              if (call->type() == Type::Ptr) {
                changed |= meet(call, &_call_result_heap);
              }
            }
          }
        }
      }
    }

    void apply(Section* section) {
      for (Block* block : *section) {
        for (Inst* inst : *block) {
          if (dynmatch(LoadInst, load, inst)) {
            Layout* ptr = at(load->ptr());
            if (ptr) {
              load->set_aliasing(group_for(ptr, load->offset()));
              if (child_of(ptr, load->offset())->singleton()) {
                load->set_flags(load->flags() | LoadFlags::Pure);
              }
            }
          } else if (dynmatch(StoreInst, store, inst)) {
            Layout* ptr = at(store->ptr());
            if (ptr) {
              store->set_aliasing(group_for(ptr, store->offset()));
            }
          }
        }
      }
    }

  public:
    Layout2Aliasing(Section* section,
                    const std::vector<Layout*>& args):
        Pass(section), _layouts(section) {
      
      for (Arg* arg : section->entry()->args()) {
        _layouts[arg] = args.at(arg->index());
      }

      find_layouts(section);
      apply(section);
    }
  };
}
