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

#include <sstream>
#include <vector>
#include <cassert>
#include <cinttypes>
#include <functional>

#define dynmatch(Type, name, value) Type* name = dynamic_cast<Type*>(value)

namespace metajit {
  enum class Type {
    Void, Bool, Int8, Int16, Int32, Int64, Ptr
  };

  inline bool is_int(Type type) {
    return type == Type::Int8 ||
           type == Type::Int16 ||
           type == Type::Int32 ||
           type == Type::Int64;
  }

  inline bool is_int_or_bool(Type type) {
    return is_int(type) || type == Type::Bool;
  }

  inline size_t type_size(Type type) {
    switch (type) {
      case Type::Void: return 0;
      case Type::Bool: return 1;
      case Type::Int8: return 1;
      case Type::Int16: return 2;
      case Type::Int32: return 4;
      case Type::Int64: return 8;
      case Type::Ptr: return sizeof(void*);
    }
    assert(false && "Unknown type");
    return 0;
  }

  inline uint64_t type_mask(Type type) {
    switch (type) {
      case Type::Void: return 0;
      case Type::Bool: return 1;
      case Type::Int64: return ~uint64_t(0);
      default:
        return (uint64_t(1) << (type_size(type) * 8)) - 1;
    }
  }
}

std::ostream& operator<<(std::ostream& stream, metajit::Type type) {
  static const char* names[] = {
    "Void", "Bool", "Int8", "Int16", "Int32", "Int64", "Ptr"
  };
  stream << names[(size_t) type];
  return stream;
}

namespace metajit {
  class Value {
  private:
    Type _type;
    size_t _name;
  public:
    Value(Type type): _type(type) {}
    virtual ~Value() {}

    Type type() const { return _type; }

    size_t name() const { return _name; }
    void set_name(size_t name) { _name = name; }

    void write_arg(std::ostream& stream) {
      stream << '%' << _name;
    }
  };

  struct InfoWriter {
  public:
    using ValueFn = std::function<void(std::ostream&, Value*)>;

    ValueFn value = nullptr;

    InfoWriter() {}
    InfoWriter(const ValueFn& _value): value(_value) {}
  };

  class Inst: public Value {
  private:
    std::vector<Value*> _args;
  public:
    Inst(Type type, const std::vector<Value*>& args):
      Value(type), _args(args) {}

    Value* arg(size_t index) const { return _args.at(index); }
    const std::vector<Value*>& args() const { return _args; }

    virtual void write(std::ostream& stream) const = 0;
    void write_args(std::ostream& stream, bool& is_first) const {
      for (Value* arg : _args) {
        if (is_first) {
          is_first = false;
        } else {
          stream << ", ";
        }
        arg->write_arg(stream);
      }
    }
  };

  class Block {
  private:
    std::vector<Inst*> _insts;
    size_t _name = 0;
  public:
    Block() {}

    auto begin() const { return _insts.begin(); }
    auto end() const { return _insts.end(); }

    Inst* at(size_t index) const { return _insts.at(index); }
    size_t size() const { return _insts.size(); }

    size_t name() const { return _name; }
    void set_name(size_t name) { _name = name; }

    void add(Inst* inst) {
      _insts.push_back(inst);
    }

    std::vector<Inst*> move_insts() {
      // TODO: How does this need to be implemented?
      std::vector<Inst*> insts = std::move(_insts);
      _insts.clear();
      return insts;
    }

    void autoname(size_t& next_name) {
      for (Inst* inst : _insts) {
        inst->set_name(next_name++);
      }
    }

    void write(std::ostream& stream, InfoWriter* info_writer = nullptr) {
      stream << "b" << _name << ":\n";
      for (Inst* inst : _insts) {
        stream << "  ";
        if (inst->type() != Type::Void) {
          inst->write_arg(stream);
          stream << " = ";
        }
        inst->write(stream);
        if (info_writer && info_writer->value) {
          stream << " ; ";
          info_writer->value(stream, inst);
        }
        stream << '\n';
      }
    }

    void write_arg(std::ostream& stream) {
      stream << 'b' << _name;
    }
  };

  template <class Self>
  class BaseFlags {
  protected:
    uint32_t _flags = 0;
  public:
    BaseFlags(uint32_t flags = 0): _flags(flags) {}

    explicit operator uint32_t() const {
      return _flags;
    }

    explicit operator uint64_t() const {
      return _flags;
    }

    bool has(Self flag) const {
      return (_flags & flag._flags) != 0;
    }
  };

  class LoadFlags final: public BaseFlags<LoadFlags> {
  public:
    enum : uint32_t {
      None = 0,
      Pure = 1 << 0
    };

    using BaseFlags<LoadFlags>::BaseFlags;

    void write(std::ostream& stream) const {
      stream << "{";
      bool is_first = true;
      #define write_flag(name) \
        if (_flags & name) { \
          if (!is_first) { stream << ", "; } \
          is_first = false; \
          stream << #name; \
        }
      
      write_flag(Pure)

      #undef write_flag
      stream << "}";
    }
  };

  class InputFlags final: public BaseFlags<InputFlags> {
  public:
    enum : uint32_t {
      None = 0,
      AssumeConst = 1 << 0
    };

    using BaseFlags<InputFlags>::BaseFlags;

    void write(std::ostream& stream) const {
      stream << "{";
      bool is_first = true;
      #define write_flag(name) \
        if (_flags & name) { \
          if (!is_first) { stream << ", "; } \
          is_first = false; \
          stream << #name; \
        }
      
      write_flag(AssumeConst)

      #undef write_flag
      stream << "}";
    }
  };

  // TODO: Refcount
  class AliasingInfo {
  private:
    std::set<size_t> _groups;
  public:
    AliasingInfo() {}
    AliasingInfo(const std::set<size_t>& groups): _groups(groups) {}

    void add_group(size_t group) {
      _groups.insert(group);
    }

    const std::set<size_t>& groups() const {
      return _groups;
    }

    bool could_alias(const AliasingInfo* other) const {
      for (size_t group : _groups) {
        if (other->_groups.find(group) != other->_groups.end()) {
          return true;
        }
      }
      return false;
    }

    void write(std::ostream& stream) const {
      stream << "{";
      bool is_first = true;
      for (size_t group : _groups) {
        if (!is_first) { stream << ", "; }
        is_first = false;
        stream << group;
      }
      stream << "}";
    }
  };
}

std::ostream& operator<<(std::ostream& stream, metajit::LoadFlags flags) {
  flags.write(stream);
  return stream;
}

std::ostream& operator<<(std::ostream& stream, metajit::InputFlags flags) {
  flags.write(stream);
  return stream;
}

namespace metajit {
  ${insts}

  class Section {
  private:
    std::vector<Block*> _blocks;
    std::vector<Type> _inputs;
    std::vector<Type> _outputs;
    size_t _name_count = 0;
  public:
    Section() {}

    auto begin() const { return _blocks.begin(); }
    auto end() const { return _blocks.end(); }

    size_t size() const { return _blocks.size(); }
    Block* operator[](size_t index) const { return _blocks.at(index); }

    Block* entry() const { return _blocks.at(0); }

    const std::vector<Type>& inputs() const { return _inputs; }
    const std::vector<Type>& outputs() const { return _outputs; }

    size_t name_count() const { return _name_count; }

    size_t add_input(Type type) {
      _inputs.push_back(type);
      return _inputs.size() - 1;
    }

    size_t add_output(Type type) {
        _outputs.push_back(type);
        return _outputs.size() - 1;
    }

    void add(Block* block) { _blocks.push_back(block); }

    void autoname() {
      _name_count = 0;
      size_t block_id = 0;
      for (Block* block : _blocks) {
        block->set_name(block_id++);
        block->autoname(_name_count);
      }
    }

    void write(std::ostream& stream, InfoWriter* info_writer = nullptr) {
      autoname();
      
      stream << "section(";
      bool is_first = true;
      for (Type type : _inputs) {
        if (is_first) {
          is_first = false;
        } else {
          stream << ", ";
        }
        stream << type;
      }
      stream << ") -> (";
      is_first = true;
      for (Type type : _outputs) {
        if (is_first) {
          is_first = false;
        } else {
          stream << ", ";
        }
        stream << type;
      }
      stream << ") {\n";
      for (Block* block : _blocks) {
        block->write(stream, info_writer);
      }
      stream << "}\n";
    }
  };

  inline bool has_side_effect(Inst* inst) {
    return dynamic_cast<StoreInst*>(inst) || dynamic_cast<OutputInst*>(inst);
  }

  inline bool is_terminator(Inst* inst) {
    return dynamic_cast<BranchInst*>(inst) ||
           dynamic_cast<JumpInst*>(inst) ||
           dynamic_cast<ExitInst*>(inst);
  }

  class Builder {
  private:
    Section* _section = nullptr;
    Block* _block = nullptr;
  public:
    Builder(Section* section): _section(section) {}

    void move_to_end(Block* block) { _block = block; }
    void insert(Inst* inst) { _block->add(inst); }

    Block* build_block() {
      Block* block = new Block();
      _section->add(block);
      return block;
    }

    ${builder}

    InputInst* build_input(Type type, InputFlags flags = InputFlags()) {
      size_t id = _section->add_input(type);
      return build_input(id, type, flags);
    }

    OutputInst* build_output(Value* value) {
      size_t id = _section->add_output(value->type());
      return build_output(value, id);
    }

    AddPtrInst* build_add_ptr(Value* ptr, uint64_t offset) {
      return build_add_ptr(ptr, build_const(Type::Int64, offset));
    }

    ShlInst* build_shl(Value* a, size_t shift) {
      assert(shift <= type_size(a->type()) * 8);
      return build_shl(a, build_const(a->type(), shift));
    }

    // Folding

    Value* fold_add(Value* a, Value* b) {
      if (dynamic_cast<ConstInst*>(a)) {
        std::swap(a, b);
      }

      if (dynmatch(ConstInst, const_b, b)) {
        if (const_b->value() == 0) {
          return a;
        }
      }

      return build_add(a, b);
    }

    Value* fold_sub(Value* a, Value* b) {
      if (dynmatch(ConstInst, const_b, b)) {
        if (const_b->value() == 0) {
          return a;
        }
      }

      return build_sub(a, b);
    }

    Value* fold_mul(Value* a, Value* b) {
      if (dynamic_cast<ConstInst*>(a)) {
        std::swap(a, b);
      }

      if (dynmatch(ConstInst, const_b, b)) {
        if (const_b->value() == 0) {
          return b;
        } else if (const_b->value() == 1) {
          return a;
        }
      }

      return build_mul(a, b);
    }

    Value* fold_and(Value* a, Value* b) {
      if (dynamic_cast<ConstInst*>(a)) {
        std::swap(a, b);
      }

      if (dynmatch(ConstInst, const_b, b)) {
        if (dynmatch(ConstInst, const_a, a)) {
          uint64_t result = const_a->value() & const_b->value();
          return build_const(a->type(), result);
        } else if (const_b->value() == type_mask(a->type())) {
          return a;
        } else if (const_b->value() == 0) {
          return const_b;
        }
      }
      return build_and(a, b);
    }

    Value* fold_or(Value* a, Value* b) {
      if (dynamic_cast<ConstInst*>(a)) {
        std::swap(a, b);
      }

      if (dynmatch(ConstInst, constant, b)) {
        if (constant->value() == 0) {
          return a;
        } else if (constant->value() == type_mask(a->type())) {
          return constant;
        }
      }
      return build_or(a, b);
    }

    Value* fold_xor(Value* a, Value* b) {
      if (dynamic_cast<ConstInst*>(a)) {
        std::swap(a, b);
      }

      if (dynmatch(ConstInst, constant, b)) {
        if (constant->value() == 0) {
          return a;
        }
      }
      return build_xor(a, b);
    }

    Value* fold_select(Value* cond, Value* true_value, Value* false_value) {
      if (dynmatch(ConstInst, const_cond, cond)) {
        if (const_cond->value() != 0) {
          return true_value;
        } else {
          return false_value;
        }
      }
      return build_select(cond, true_value, false_value);
    }

    Value* fold_add_ptr(Value* ptr, Value* offset) {
      if (dynmatch(ConstInst, constant, offset)) {
        if (constant->value() == 0) {
          return ptr;
        }
      }
      return build_add_ptr(ptr, offset);
    }

    Value* fold_add_ptr(Value* ptr, uint64_t offset) {
      if (offset == 0) {
        return ptr;
      }
      return fold_add_ptr(ptr, build_const(Type::Int64, offset));
    }

    Value* fold_resize_u(Value* a, Type type) {
      if (a->type() == type) {
        return a;
      } else if (dynmatch(ConstInst, constant, a)) {
        uint64_t value = constant->value() & type_mask(type);
        return build_const(type, value);
      }
      return build_resize_u(a, type);
    }

    Value* fold_shl(Value* a, Value* b) {
      if (dynmatch(ConstInst, const_b, b)) {
        if (dynmatch(ConstInst, const_a, a)) {
          uint64_t result = const_a->value() << const_b->value();
          result &= type_mask(a->type());
          return build_const(a->type(), result);
        } else if (const_b->value() == 0) {
          return a;
        }
      }
      return build_shl(a, b);
    }

    Value* fold_shl(Value* a, size_t shift) {
      assert(shift <= type_size(a->type()) * 8);
      return fold_shl(a, build_const(a->type(), shift));
    }

    Value* fold_shr_u(Value* a, Value* b) {
      if (dynmatch(ConstInst, constant, b)) {
        if (constant->value() == 0) {
          return a;
        }
      }
      return build_shr_u(a, b);
    }

    Value* fold_shr_s(Value* a, Value* b) {
      if (dynmatch(ConstInst, constant, b)) {
        if (constant->value() == 0) {
          return a;
        }
      }
      return build_shr_s(a, b);
    }
  };

  struct Pointer {
    Value* base = nullptr;
    uint64_t offset;

    Pointer(): base(nullptr), offset(0) {}
    Pointer(Value* _base, uint64_t _offset): base(_base), offset(_offset) {}

    Pointer operator+(uint64_t other) const {
      if (base == nullptr) {
        return Pointer();
      }
      return Pointer(base, offset + other);
    }

    bool operator==(const Pointer& other) const {
      return base == other.base && offset == other.offset;
    }

    bool operator!=(const Pointer& other) const {
      return !(*this == other);
    }

    bool operator<(const Pointer& other) const {
      return base < other.base || (base == other.base && offset < other.offset);
    }
  };

  class TraceBuilder: public Builder {
  private:
    // We perform optimizations during trace generation
    
    struct GroupState {
      Value* base = nullptr;
      std::map<size_t, Value*> stores;

      GroupState() {}

      Value* load(const Pointer& pointer, Type type) {
        if (base == pointer.base &&
            base != nullptr &&
            pointer.base != nullptr &&
            stores.find(pointer.offset) != stores.end() &&
            stores.at(pointer.offset)->type() == type) {
          return stores.at(pointer.offset);
        }
        return nullptr;
      }

      void store(const Pointer& pointer, Value* value) {
        if (pointer.base == nullptr) {
          base = nullptr;
          stores.clear();
          return;
        }

        if (base != pointer.base) {
          base = pointer.base;
          stores.clear();
        }
        // TODO: Overlapping
        stores[pointer.offset] = value;
      }
    };

    std::unordered_map<Value*, Pointer> _pointers;
    std::unordered_map<size_t, GroupState> _memory;
    std::unordered_map<size_t, InputInst*> _inputs;

    Pointer pointer(Value* value) {
      if (_pointers.find(value) == _pointers.end()) {
        _pointers[value] = Pointer(value, 0);
      }
      return _pointers.at(value);
    }

    Value* load(Pointer pointer, Type type, AliasingInfo* aliasing) {
      Value* same_value;
      bool has_same_value = false;
      for (size_t group : aliasing->groups()) {
        Value* value = _memory[group].load(pointer, type);
        if (has_same_value) {
          if (value != same_value) {
            same_value = nullptr;
            break;
          }
        } else {
          same_value = value;
        }
      }

      return same_value;
    }
  public:
    using Builder::Builder;

    // Use folding versions so that we get short-circuit behavior and simplifications

    Value* build_add(Value* a, Value* b) {
      return Builder::fold_add(a, b);
    }

    Value* build_sub(Value* a, Value* b) {
      return Builder::fold_sub(a, b);
    }

    Value* build_mul(Value* a, Value* b) {
      return Builder::fold_mul(a, b);
    }

    Value* build_select(Value* cond, Value* true_value, Value* false_value) {
      return Builder::fold_select(cond, true_value, false_value);
    }

    Value* build_and(Value* a, Value* b) {
      return Builder::fold_and(a, b);
    }

    Value* build_or(Value* a, Value* b) {
      return Builder::fold_or(a, b);
    }

    Value* build_xor(Value* a, Value* b) {
      return Builder::fold_xor(a, b);
    }

    // We deduplicate input building to track pointers

    Value* build_input(size_t id, Type type, InputFlags flags = InputFlags()) {
      if (_inputs.find(id) == _inputs.end()) {
        _inputs[id] = Builder::build_input(id, type, flags);
      }
      assert(_inputs.at(id)->type() == type);
      return _inputs.at(id);
    }

    // We override add_ptr to track pointers

    Value* build_add_ptr(Value* ptr, Value* offset) {
      Value* result = Builder::fold_add_ptr(ptr, offset);

      if (dynmatch(ConstInst, constant, offset)) {
        _pointers[result] = pointer(ptr) + constant->value();
      }

      return result;
    }

    // We override load/store to do simple load/store forwarding

    Value* build_load(Value* ptr, Type type, LoadFlags flags, AliasingInfo* aliasing) {
      if (aliasing) {
        if (Value* value = load(pointer(ptr), type, aliasing)) {
          return value;
        }
      }

      return Builder::build_load(ptr, type, flags, aliasing);
    }

    Value* build_store(Value* ptr, Value* value, AliasingInfo* aliasing) {
      if (aliasing == nullptr) {
        _memory.clear();
      } else {
        for (size_t group : aliasing->groups()) {
          _memory[group].store(pointer(ptr), value);
        }
      }

      return Builder::build_store(ptr, value, aliasing);
    }

  };

  ${capi}


  template<class T>
  class ValueMap {
  private:
    T* _data = nullptr;
    size_t _size = 0;
  public:
    ValueMap() {}
    ValueMap(Section* section) { init(section); }

    ValueMap(const ValueMap<T>&) = delete;
    ValueMap<T>& operator=(const ValueMap<T>&) = delete;

    ~ValueMap() { if (_data) { delete[] _data; } }

    void init(Section* section) {
      assert(_data == nullptr);
      _size = section->name_count();
      _data = new T[_size]();
    }

    T& at(Value* value) {
      assert(value->name() < _size);  
      return _data[value->name()];
    }

    T& operator[](Value* value) { return at(value); }
  };

  template <class Self>
  class Pass {
  public:
    Pass(Section* section) {}

    template <class... Args>
    static void run(Section* section, Args... args) {
      Self self(section, args...);
    }
  };

  class DeadCodeElim: public Pass<DeadCodeElim> {
  public:
    DeadCodeElim(Section* section): Pass(section) {
      section->autoname();
      ValueMap<bool> used(section);

      for (size_t block_id = section->size(); block_id-- > 0; ) {
        Block* block = (*section)[block_id];
        for (size_t inst_id = block->size(); inst_id-- > 0; ) {
          Inst* inst = block->at(inst_id);
          if (has_side_effect(inst) || is_terminator(inst) || used[inst]) {
            used[inst] = true;
            for (Value* arg : inst->args()) {
              used[arg] = true;
            }
          }
        }
      }

      for (Block* block : *section) {
        std::vector<Inst*> insts = block->move_insts();
        for (Inst* inst : insts) {
          if (used[inst]) {
            block->add(inst);
          } else {
            delete inst;
          }
        }
      }
    }
  };

  class DeadStoreElim: public Pass<DeadStoreElim> {
  private:
    ValueMap<Pointer> _pointers;

    struct Interval {
      size_t min;
      size_t max; // Exclusive

      Interval(size_t offset, Type type) {
        min = offset;
        max = offset + type_size(type);
      }

      bool intersects(const Interval& other) const {
        return max > other.min && min < other.max;
      }
    };

    bool could_alias(LoadInst* load, StoreInst* store) {
      if (load->aliasing() != nullptr &&
          store->aliasing() != nullptr &&
          !load->aliasing()->could_alias(store->aliasing())) {
        return false;
      }

      Pointer load_ptr = _pointers[load->ptr()];
      Pointer store_ptr = _pointers[store->ptr()];

      if (load_ptr.base != store_ptr.base) {
        return true;
      }

      Interval load_interval(load_ptr.offset, load->type());
      Interval store_interval(store_ptr.offset, store->value()->type());

      return load_interval.intersects(store_interval);
    }
  public:
    DeadStoreElim(Section* section): Pass(section) {
      section->autoname();
      _pointers.init(section);
      
      for (Block* block : *section) {
        for (Inst* inst : *block) {
          if (dynmatch(AddPtrInst, add_ptr, inst)) {
            if (dynmatch(ConstInst, constant, add_ptr->arg(1))) {
              _pointers[inst] = _pointers[add_ptr->arg(0)] + constant->value();
            } else {
              _pointers[inst] = Pointer(inst, 0);
            }
          } else {
            _pointers[inst] = Pointer(inst, 0);
          }
        }
      }

      for (Block* block : *section) {
        std::set<Inst*> unused;
        std::map<Pointer, StoreInst*> last_store;

        std::vector<Inst*> insts = block->move_insts();

        for (Inst* inst : insts) {
          if (dynmatch(StoreInst, store, inst)) {
            Pointer pointer = _pointers[store->ptr()];
            last_store[pointer] = store;
            unused.insert(inst);
          } else if (dynmatch(LoadInst, load, inst)) {
            for (const auto& [pointer, store] : last_store) {
              if (could_alias(load, store)) {
                unused.erase(store);
              }
            }
          }
        }

        for (const auto& [pointer, store] : last_store) {
          unused.erase(store);
        }

        for (Inst* inst : insts) {
          if (unused.find(inst) == unused.end()) {
            block->add(inst);
          } else {
            delete inst;
          }
        }
      }
    }
  };

  class KnownBits: public Pass<KnownBits> {
  public:
    struct Bits {
      Type type = Type::Void;
      uint64_t mask = 0;
      uint64_t value = 0;
      
      Bits() {}
      Bits(Type _type, uint64_t _mask, uint64_t _value):
        type(_type), mask(_mask), value(_value) {}

      std::optional<bool> at(size_t bit) const {
        if (mask & (uint64_t(1) << bit)) {
          return (value & (uint64_t(1) << bit)) != 0;
        }
        return {};
      }

      bool is_const() const {
        return mask == type_mask(type);
      }

      Bits operator&(const Bits& other) const {
        return Bits(
          type,
          (mask & other.mask) | (mask & ~value) | (other.mask & ~other.value),
          value & other.value
        );
      }

      Bits operator|(const Bits& other) const {
        return Bits(
          type,
          (mask & other.mask) | (mask & value) | (other.mask & other.value),
          value | other.value
        );
      }

      Bits shl(size_t shift) const {
        return Bits(
          type,
          ((mask << shift) | ((uint64_t(1) << shift) - 1)) & type_mask(type),
          (value << shift) & type_mask(type)
        );
      }

      Bits shr_u(size_t shift) const {
        return Bits(
          type,
          (mask >> shift) | (type_mask(type) & ~(type_mask(type) >> shift)),
          value >> shift
        );
      }

      #define shift_by_const(name) \
        Bits name(const Bits& other) const { \
          if (other.is_const()) { \
            return name(other.value); \
          } \
          return Bits(type, 0, 0); \
        }
      
      shift_by_const(shl)
      shift_by_const(shr_u)

      #undef shift_by_const

      Bits resize_u(Type to) const {
        return Bits(
          to,
          (mask & type_mask(type) & type_mask(to)) | (type_mask(to) & ~type_mask(type)),
          value & type_mask(type) & type_mask(to)
        );
      }

      void write(std::ostream& stream) const {
        size_t bits = type == Type::Bool ? 1 : type_size(type) * 8;
        for (size_t it = bits; it-- > 0; ) {
          std::optional<bool> bit = at(it);
          if (bit.has_value()) {
            stream << (bit.value() ? '1' : '0');
          } else {
            stream << '_';
          }
        }
      }   
    };

  private:
    Section* _section;
    ValueMap<Bits> _values;
  public:
    KnownBits(Section* section): Pass(section), _section(section) {
      section->autoname();
      _values.init(section);

      for (Block* block : *section) {
        for (Inst* inst : *block) {
          if (dynmatch(ConstInst, constant, inst)) {
            _values[inst] = Bits(
              constant->type(),
              type_mask(constant->type()),
              constant->value()
            );
          } else if (dynmatch(ResizeUInst, resize_u, inst)) {
            Bits a = _values[resize_u->arg(0)];
            _values[inst] = a.resize_u(resize_u->type());
          }

          #define binop(name, expr) \
            else if (dynmatch(name, binop, inst)) { \
              Bits a = _values[binop->arg(0)]; \
              Bits b = _values[binop->arg(1)]; \
              _values[inst] = expr; \
            }
          
          binop(AndInst, a & b)
          binop(OrInst, a | b)
          
          binop(ShlInst, a.shl(b))
          binop(ShrUInst, a.shr_u(b))

          #undef binop

          else {
            _values[inst] = Bits(inst->type(), 0, 0);
          }
        }
      }
    }

    void write(std::ostream& stream) {
      InfoWriter info_writer([&](std::ostream& stream, Value* value){
        _values[value].write(stream);
      });
      _section->write(stream, &info_writer);
    }
  };
}

