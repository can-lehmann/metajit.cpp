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
      std::optional<uint64_t> _size = std::nullopt;
    public:
      Layout(bool singleton): _singleton(singleton) {}
      virtual ~Layout() = default;

      bool singleton() const { return _singleton; }
      Layout* set_singleton(bool singleton) { _singleton = singleton; return this; }

      std::optional<uint64_t> size() const { return _size; }
      Layout* set_size(std::optional<uint64_t> size) { _size = size; return this; }

      virtual Layout* deref(std::optional<uint64_t> offset) = 0;
    };

    class Record: public Layout {
    private:
      std::map<uint64_t, Layout*> _fields;
    public:
      Record(bool singleton, std::map<uint64_t, Layout*> fields):
        Layout(singleton), _fields(std::move(fields)) {}

      void add_field(uint64_t offset, Layout* layout) {
        _fields[offset] = layout;
      }

      Layout* at(uint64_t offset) const {
        auto it = _fields.find(offset);
        if (it == _fields.end()) {
          return nullptr;
        }
        return it->second;
      }

      Layout* deref(std::optional<uint64_t> offset) override {
        if (!offset) {
          throw std::runtime_error("Layout2Aliasing: Record layout requires an offset");
        }
        Layout* field = at(*offset);
        if (!field) {
          throw std::runtime_error("Layout2Aliasing: no declared field at offset " + std::to_string(*offset));
        }
        return field;
      }
    };

    class Array: public Layout {
    private:
      Layout* _element;
    public:
      Array(bool singleton, Layout* element):
        Layout(singleton), _element(element) {}

      Layout* element() const { return _element; }

      Layout* deref(std::optional<uint64_t> offset) override {
        return _element;
      }
    };

    class Heap: public Layout {
    public:
      Heap(): Layout(false) {}

      Layout* deref(std::optional<uint64_t> offset) override {
        return this;
      }
    };

    class LayoutBuilder {
    private:
      Allocator* _allocator = nullptr;
      bool _owns_allocator = false;
      Heap* _heap = nullptr;
    public:
      LayoutBuilder(): _allocator(new ArenaAllocator()), _owns_allocator(true) {}
      LayoutBuilder(Allocator& allocator): _allocator(&allocator), _owns_allocator(false) {}

      ~LayoutBuilder() {
        if (_owns_allocator && _allocator) {
          delete _allocator;
        }
      }

      Heap* heap() {
        if (!_heap) {
          _heap = new (_allocator->alloc<Heap>()) Heap();
        }
        return _heap;
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
    // bottom < layout + offset < layout + unknown offset = top
    struct Pointer {
      Layout* layout = nullptr;
      std::optional<uint64_t> offset = std::nullopt;

      Pointer() = default;
      Pointer(Layout* layout, std::optional<uint64_t> offset = std::nullopt):
        layout(layout), offset(offset) {
        
        if (dynamic_cast<Heap*>(layout) || dynamic_cast<Array*>(layout)) {
          offset = std::nullopt;
        }
      }
     
      // Currently, we canonicalize in constructor
      Pointer canonical() const { return *this; }

      bool is_bottom() const { return layout == nullptr; }
      bool is_top() const { return layout != nullptr && !offset.has_value(); }

      bool meet(const Pointer& other) {
        if (other.is_bottom()) {
          return false;
        }
        if (is_bottom()) {
          *this = other;
          return true;
        }
        if (is_top()) {
          return false;
        }
        if (other.is_top()) {
          offset = std::nullopt;
          return true;
        }
        if (layout != other.layout || *offset != *other.offset) {
          throw std::runtime_error("Layout2Aliasing: conflicting pointer meet");
        }
        return false;
      }
      
      Pointer add_offset(uint64_t _offset) const {
        if (is_bottom() || is_top()) {
          return *this;
        }
        return Pointer(layout, *offset + _offset);
      }

      Pointer deref() const {
        assert(!is_bottom());
        return Pointer(layout->deref(offset), 0);
      }

      bool operator<(const Pointer& other) const {
        if (layout != other.layout) {
          return layout < other.layout;
        }
        return offset < other.offset;
      }

      bool is_in_bounds() const {
        if (is_bottom() || is_top() || !layout->size().has_value()) {
          return false;
        }
        return *offset < *layout->size();
      }
    };

    NameMap<Pointer> _pointers;
    std::map<Pointer, AliasingGroup> _group_ids;
    AliasingGroup _next_group = 1;
    AliasingGroup _next_exact_group = -1;
    inline static Heap _call_result_heap;

    AliasingGroup group_for(Pointer ptr) {
      if (dynamic_cast<Heap*>(ptr.layout)) {
        return 0;
      }
      ptr = ptr.canonical();
      if (_group_ids.find(ptr) == _group_ids.end()) {
        if (dynamic_cast<Record*>(ptr.layout) && ptr.layout->singleton()) {
          _group_ids[ptr] = _next_exact_group--;
        } else {
          _group_ids[ptr] = _next_group++;
        }
      }
      return _group_ids[ptr];
    }

    void find_layouts(Section* section) {
      bool changed = true;
      while (changed) {
        changed = false;
        for (Block* block : *section) {
          for (Inst* inst : *block) {
            if (dynmatch(AddPtrInst, add_ptr, inst)) {
              Pointer ptr = at(add_ptr->ptr());
              if (ptr.offset.has_value()) {
                if (dynmatch(Const, constant, add_ptr->offset())) {
                  *ptr.offset += constant->value();
                } else {
                  ptr.offset = std::nullopt;
                }
              }
              changed |= _pointers[add_ptr].meet(ptr);
            } else if (dynmatch(LoadInst, load, inst)) {
              Pointer ptr = at(load->ptr()).add_offset(load->offset());
              if (!ptr.is_bottom() && load->type() == Type::Ptr) {
                changed |= _pointers[load].meet(ptr.deref());
              }
            } else if (dynmatch(JumpInst, jump, inst)) {
              for (Arg* arg : jump->block()->args()) {
                changed |= _pointers[arg].meet(at(jump->arg(arg->index())));
              }
            } else if (dynmatch(CallInst, call, inst)) {
              if (call->type() == Type::Ptr) {
                changed |= _pointers[call].meet(Pointer(&_call_result_heap));
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
            Pointer ptr = at(load->ptr()).add_offset(load->offset());
            if (!ptr.is_bottom()) {
              load->set_aliasing(group_for(ptr));
              if (ptr.deref().layout && ptr.deref().layout->singleton()) {
                load->set_flags(load->flags() | LoadFlags::Pure);
              }
              if (ptr.is_in_bounds()) {
                load->set_flags(load->flags() | LoadFlags::InBounds);
              }
            }
          } else if (dynmatch(StoreInst, store, inst)) {
            Pointer ptr = at(store->ptr()).add_offset(store->offset());
            if (!ptr.is_bottom()) {
              store->set_aliasing(group_for(ptr));
            }
          }
        }
      }
    }

  public:
    Layout2Aliasing(Section* section,
                    const std::vector<Layout*>& args):
        Pass(section), _pointers(section) {

      for (Arg* arg : section->entry()->args()) {
        _pointers[arg] = Pointer(args.at(arg->index()), 0);
      }

      find_layouts(section);
      apply(section);
    }

    Pointer at(Value* value) const {
      if (value->is_named()) {
        return _pointers[(NamedValue*) value];
      } else {
        return Pointer();
      }
    }
  };
}
