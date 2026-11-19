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

#include <algorithm>
#include <new>
#include <map>
#include <unordered_map>
#include <vector>
#include <stdexcept>

#include "jitir.hpp"

namespace metajit {
  class Layout {
  private:
    bool _singleton = false;
    bool _pure = false;
    std::optional<uint64_t> _size = std::nullopt;
  public:
    Layout() {}
    virtual ~Layout() = default;

    bool singleton() const { return _singleton; }
    Layout* set_singleton(bool singleton) { _singleton = singleton; return this; }

    bool pure() const { return _pure; }
    Layout* set_pure(bool pure) { _pure = pure; return this; }

    std::optional<uint64_t> size() const { return _size; }
    Layout* set_size(std::optional<uint64_t> size) { _size = size; return this; }

    virtual Layout* deref(std::optional<uint64_t> offset) = 0;
  };

  class RecordLayout: public Layout {
  private:
    std::map<uint64_t, Layout*> _fields;
  public:
    RecordLayout(std::map<uint64_t, Layout*> fields):
      Layout(), _fields(std::move(fields)) {}

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

  class ArrayLayout: public Layout {
  private:
    Layout* _element;
  public:
    ArrayLayout(Layout* element):
      Layout(), _element(element) {}

    Layout* element() const { return _element; }

    Layout* deref(std::optional<uint64_t> offset) override {
      return _element;
    }
  };

  class HeapLayout: public Layout {
  public:
    HeapLayout(): Layout() {}

    Layout* deref(std::optional<uint64_t> offset) override {
      return this;
    }
  };

  class LayoutBuilder {
  private:
    Allocator* _allocator = nullptr;
    bool _owns_allocator = false;
    HeapLayout* _heap = nullptr;
    bool _pure = false;
  public:
    LayoutBuilder(): _allocator(new Allocator()), _owns_allocator(true) {}
    LayoutBuilder(Allocator& allocator): _allocator(&allocator), _owns_allocator(false) {}

    ~LayoutBuilder() {
      if (_owns_allocator && _allocator) {
        delete _allocator;
      }
    }

    bool pure() const { return _pure; }
    void set_pure(bool pure) { _pure = pure; }

    HeapLayout* heap() {
      if (!_heap) {
        _heap = new (_allocator->alloc<HeapLayout>()) HeapLayout();
      }
      return _heap;
    }

    ArrayLayout* array(bool singleton, Layout* element) {
      ArrayLayout* array = new (_allocator->alloc<ArrayLayout>()) ArrayLayout(element);
      array->set_pure(_pure);
      array->set_singleton(singleton);
      return array;
    }

    RecordLayout* record(bool singleton, std::map<uint64_t, Layout*> fields) {
      RecordLayout* record = new (_allocator->alloc<RecordLayout>()) RecordLayout(std::move(fields));
      record->set_pure(_pure);
      record->set_singleton(singleton);
      return record;
    }

    ArrayLayout* array(Layout* element) { return array(false, element); }
    ArrayLayout* array_singleton(Layout* element) { return array(true, element); }
    RecordLayout* record(std::map<uint64_t, Layout*> fields) { return record(false, std::move(fields)); }
    RecordLayout* record_singleton(std::map<uint64_t, Layout*> fields) { return record(true, std::move(fields)); }
  };

  class PointerLayouts {
  public:
    using BoundedOffsets = std::unordered_map<Value*, uint64_t>;
    // bottom < layout + offset < layout + unknown offset = top
    struct Pointer {
      Layout* layout = nullptr;
      std::optional<uint64_t> offset = std::nullopt;

      uint64_t extent = 0;

      Pointer() = default;
      Pointer(Layout* layout, std::optional<uint64_t> offset = std::nullopt):
        layout(layout), offset(offset) {
        
        if (dynamic_cast<HeapLayout*>(layout) || dynamic_cast<ArrayLayout*>(layout)) {
          this->offset = std::nullopt;
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
          extent = 0;
          return true;
        }
        if (layout != other.layout || *offset != *other.offset || extent != other.extent) {
          throw std::runtime_error("Layout2Aliasing: conflicting pointer meet");
        }
        return false;
      }
      
      Pointer add_offset(uint64_t _offset) const {
        if (is_bottom() || is_top()) {
          return *this;
        }
        Pointer result = *this;
        *result.offset += _offset;
        return result;
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

      bool is_in_bounds(size_t access_size) const {
        if (is_bottom() || is_top() || !layout->size().has_value()) {
          return false;
        }
        size_t size = *layout->size();
        return *offset <= size && extent <= size - *offset && access_size <= size - *offset - extent;
      }
    };

  private:
    inline static HeapLayout _call_result_heap;

    Section* _section = nullptr;
    NameMap<Pointer> _pointers;
  public:
    PointerLayouts(Section* section,
                   const std::vector<Layout*>& args,
                   const BoundedOffsets& bounded_offsets = {}):
        _section(section) {
      _section->autoname();
      _pointers.init(section);


      for (Arg* arg : section->entry()->args()) {
        _pointers[arg] = Pointer(args.at(arg->index()), 0);
      }

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
                } else if (bounded_offsets.find(add_ptr) != bounded_offsets.end()) {
                  ptr.extent += bounded_offsets.at(add_ptr);
                } else {
                  if (dynamic_cast<RecordLayout*>(ptr.layout)) {
                    throw std::runtime_error("Layout2Aliasing: Record layout requires a constant offset");
                  }
                  ptr.offset = std::nullopt;
                  ptr.extent = 0;
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
            } else if (dynamic_cast<AssumeConstInst*>(inst) || dynamic_cast<PromoteInst*>(inst)) {
              if (inst->type() == Type::Ptr) {
                changed |= _pointers[inst].meet(at(inst->arg(0)));
              }
            } else if (dynamic_cast<SelectInst*>(inst) && inst->type() == Type::Ptr) {
              throw std::runtime_error("Layout2Aliasing: pointer Select is unsupported");
            }
          }
        }
      }
    }

    Pointer at(Value* value) const {
      if (value->is_named()) {
        return _pointers[(NamedValue*) value];
      } else {
        return Pointer();
      }
    }

    void mark_impure() {
      for (Block* block : *_section) {
        for (Inst* inst : *block) {
          if (dynmatch(StoreInst, store, inst)) {
            Pointer ptr = at(store->ptr()).add_offset(store->offset());
            if (!ptr.is_bottom()) {
              ptr.layout->set_pure(false);
            }
          }
        }
      }
    }
  };

  class Layout2Aliasing: public Pass<Layout2Aliasing> {
  private:
    using Pointer = PointerLayouts::Pointer;
    
    PointerLayouts& _pointers;
    std::map<Pointer, AliasingGroup> _group_ids;
    AliasingGroup _next_group = 1;
    AliasingGroup _next_exact_group = -1;
    std::unordered_map<Inst*, AliasingGroup> _overlapping_groups;

    struct Access {
      Inst* inst;
      uint64_t begin;
      uint64_t end;
      bool bounded;
    };

    void collect_access(std::map<Layout*, std::vector<Access>>& accesses,
                        Inst* inst, Pointer ptr, Type type) {
      if (ptr.offset && dynamic_cast<RecordLayout*>(ptr.layout)) {
        accesses[ptr.layout].push_back(
          {inst, *ptr.offset, *ptr.offset + ptr.extent + type_size(type), ptr.extent != 0});
      }
    }

    static bool access_order(const Access& a, const Access& b) {
      return a.begin < b.begin;
    }

    void group_overlapping_accesses(Section* section) {
      std::map<Layout*, std::vector<Access>> accesses;
      for (Block* block : *section) {
        for (Inst* inst : *block) {
          if (dynmatch(LoadInst, load, inst)) {
            collect_access(accesses, inst, _pointers.at(load->ptr()).add_offset(load->offset()), load->type());
          } else if (dynmatch(StoreInst, store, inst)) {
            collect_access(accesses, inst, _pointers.at(store->ptr()).add_offset(store->offset()), store->value()->type());
          }
        }
      }
      for (auto& [layout, ranges] : accesses) {
        std::stable_sort(ranges.begin(), ranges.end(), access_order);
        for (size_t first = 0; first < ranges.size();) {
          uint64_t end = ranges[first].end;
          bool overlapping = ranges[first].bounded;
          size_t next = first + 1;
          while (next < ranges.size() && ranges[next].begin < end) {
            overlapping |= ranges[next].bounded || ranges[next].begin != ranges[first].begin ||
                           ranges[next].end != ranges[first].end;
            end = std::max(end, ranges[next].end);
            ++next;
          }
          if (overlapping) {
            AliasingGroup group = _next_group++;
            for (size_t i = first; i < next; ++i) {
              _overlapping_groups[ranges[i].inst] = group;
            }
          }
          first = next;
        }
      }
    }

    AliasingGroup group_for(Inst* inst, Pointer ptr) {
      if (_overlapping_groups.find(inst) != _overlapping_groups.end()) {
        return _overlapping_groups.at(inst);
      }
      if (dynamic_cast<HeapLayout*>(ptr.layout)) {
        return 0;
      }
      ptr = ptr.canonical();
      if (_group_ids.find(ptr) == _group_ids.end()) {
        if (dynamic_cast<RecordLayout*>(ptr.layout) && ptr.layout->singleton()) {
          _group_ids[ptr] = _next_exact_group--;
        } else {
          _group_ids[ptr] = _next_group++;
        }
      }
      return _group_ids[ptr];
    }

    void apply(Section* section) {
      group_overlapping_accesses(section);
      for (Block* block : *section) {
        for (Inst* inst : *block) {
          if (dynmatch(LoadInst, load, inst)) {
            Pointer ptr = _pointers.at(load->ptr()).add_offset(load->offset());
            if (!ptr.is_bottom()) {
              load->set_aliasing(group_for(inst, ptr));
              if (load->type() == Type::Ptr && ptr.deref().layout && ptr.deref().layout->singleton()) {
                load->set_flags(load->flags() | LoadFlags::Pure);
              }
              if (ptr.is_in_bounds(type_size(load->type()))) {
                load->set_flags(load->flags() | LoadFlags::InBounds);
              }
              if (ptr.layout->pure()) {
                load->set_flags(load->flags() | LoadFlags::Pure);
              }
            }
          } else if (dynmatch(StoreInst, store, inst)) {
            Pointer ptr = _pointers.at(store->ptr()).add_offset(store->offset());
            if (!ptr.is_bottom()) {
              store->set_aliasing(group_for(inst, ptr));
            }
          }
        }
      }
    }

  public:
    Layout2Aliasing(Section* section, PointerLayouts* pointers):
        Pass(section), _pointers(*pointers) {
      apply(section);
    }
  };
}
