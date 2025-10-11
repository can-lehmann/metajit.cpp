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
#include <unordered_map>
#include <unordered_set>

#define dynmatch(Type, name, value) Type* name = dynamic_cast<Type*>(value)

namespace metajit {

  class ArenaAllocator {
  private:
    struct Chunk {
      Chunk* next = nullptr;
      uint8_t data[0];
    };

    static constexpr size_t CHUNK_SIZE = 1024 * 1024; // 1 MiB
    static constexpr size_t USABLE_SIZE = CHUNK_SIZE - sizeof(Chunk);

    Chunk* _first = nullptr;
    Chunk* _current = nullptr;

    size_t _left = 0;
    uint8_t* _ptr = nullptr;

    inline size_t align_pad(void* ptr, size_t align) {
      size_t delta = (uintptr_t) ptr % align;
      return delta ? align - delta : 0;
    }
  public:
    ArenaAllocator() {
      _first = (Chunk*) malloc(CHUNK_SIZE);
      new (_first) Chunk();
      _current = _first;
    }

    ~ArenaAllocator() {
      Chunk* chunk = _first;
      while (chunk) {
        Chunk* next = chunk->next;
        free(chunk);
        chunk = next;
      }
    }

    void* alloc(size_t size, size_t align) {
      assert(size <= USABLE_SIZE);

      size_t align_padding = align_pad(_ptr, align);

      if (__builtin_expect(_left - align_padding < size, 0)) {
        if (_current->next) {
          _current = _current->next;
        } else {
          Chunk* chunk = (Chunk*) malloc(CHUNK_SIZE);
          new (chunk) Chunk();
          _current->next = chunk;
          _current = chunk;  
        }
        _left = USABLE_SIZE;
        _ptr = _current->data;
        align_padding = align_pad(_ptr, align);
      }

      _ptr += align_padding;
      _left -= align_padding;
      void* ptr = (void*) _ptr;
      _ptr += size;
      _left -= size; 
      return ptr;
    }

    void dealloc_all() {
      _current = _first;
      _ptr = _first->data;
      _left = USABLE_SIZE;
    }
  };

  // WARNING: Does not deallocate, only use for testing
  class MallocAllocator {
  public:
    MallocAllocator() {}
    ~MallocAllocator() {}

    void* alloc(size_t size, size_t align) {
      return malloc(size);
    }
  };

  using Allocator = ArenaAllocator;

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
      default:
        if (type_size(type) >= 8) {
          return ~uint64_t(0);
        } else {
          return (uint64_t(1) << (type_size(type) * 8)) - 1;
        }
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

    void write_arg(std::ostream& stream) const {
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

  class Block;
  
  template <class T>
  class Span {
  private:
    T* _data;
    size_t _size = 0;
  public:
    Span(T* data, size_t size): _data(data), _size(size) {}
    
    template <class Ptr>
    static Span<T> offset(Ptr* base, size_t offset, size_t size) {
      return Span<T>((T*) ((uint8_t*) base + offset), size);
    }

    template <class Ptr>
    static Span<T> trailing(Ptr* base, size_t size) {
      return Span<T>((T*) ((uint8_t*) base + sizeof(Ptr)), size);
    }

    T* data() const { return _data; }

    size_t size() const { return _size; }
    
    inline T at(size_t index) const {
      assert(index < _size);
      return _data[index];
    }

    inline T& at(size_t index) {
      assert(index < _size);
      return _data[index];
    }

    inline T operator[](size_t index) const { return at(index); }
    inline T& operator[](size_t index) { return at(index); }

    T* begin() const { return _data; }
    T* end() const { return _data + _size; }

    Span zeroed() const {
      memset(_data, 0, sizeof(T) * _size);
      return *this;
    }

    Span with(size_t index, const T& value) const {
      assert(index < _size);
      _data[index] = value;
      return *this;
    }
  };

  class Inst: public Value {
  private:
    Span<Value*> _args;
  public:
    Inst(Type type, const Span<Value*>& args):
      Value(type), _args(args) {}

    const Span<Value*>& args() const { return _args; }
    size_t arg_count() const { return _args.size(); }
    Value* arg(size_t index) const { return _args.at(index); }

    void set_arg(size_t index, Value* value) {
      assert(!value || !_args.at(index) || value->type() == _args.at(index)->type());
      _args[index] = value;
    }

    virtual void write(std::ostream& stream) const = 0;
    void write_args(std::ostream& stream, bool& is_first) const {
      for (Value* arg : _args) {
        if (is_first) {
          is_first = false;
        } else {
          stream << ", ";
        }
        if (arg == nullptr) {
          stream << "<NULL>";
        } else {
          arg->write_arg(stream);
        }
      }
    }

    bool has_side_effect() const;
    bool is_terminator() const;
    std::vector<Block*> successor_blocks() const;
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

    Inst* terminator() const;
    std::vector<Block*> successors() const;

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
  using AliasingGroup = int32_t;

  ${insts}

  class PhiInst: public Inst {
  private:
    Span<Block*> _blocks;
  public:
    PhiInst(Type type, size_t incoming_count):
      Inst(type, Span<Value*>::trailing(this, incoming_count).zeroed()),
      _blocks(Span<Block*>::offset(this, sizeof(PhiInst) + sizeof(Value*) * incoming_count, incoming_count).zeroed()) {}

    Block* block(size_t index) const {
      return _blocks.at(index);
    }

    void set_incoming(size_t index, Value* value, Block* from) {
      set_arg(index, value);
      _blocks[index] = from;
    }

    void write(std::ostream& stream) const override {
      stream << "phi ";
      bool is_first = true;
      for (size_t it = 0; it < arg_count(); it++) {
        if (is_first) {
          is_first = false;
        } else {
          stream << ", ";
        }
        _blocks[it]->write_arg(stream);
        stream << " -> ";
        arg(it)->write_arg(stream);
      }
    }

    bool verify(const std::set<Block*>& incoming,
                std::ostream& errors) const {
      
      std::set<Block*> blocks;
      blocks.insert(_blocks.begin(), _blocks.end());

      if (blocks != incoming) {
        errors << "Phi node ";
        write_arg(errors);
        errors << " has incorrect incoming blocks\n";
        return true;
      }
      return false;
    }
  };

  bool Inst::has_side_effect() const {
    return dynamic_cast<const StoreInst*>(this) ||
           dynamic_cast<const OutputInst*>(this);
  }

  bool Inst::is_terminator() const {
    return dynamic_cast<const BranchInst*>(this) ||
           dynamic_cast<const JumpInst*>(this) ||
           dynamic_cast<const ExitInst*>(this);
  }

  std::vector<Block*> Inst::successor_blocks() const {
    if (dynmatch(const BranchInst, branch, this)) {
      return { branch->true_block(), branch->false_block() };
    } else if (dynmatch(const JumpInst, jump, this)) {
      return { jump->block() };
    } else {
      return {};
    }
  }

  Inst* Block::terminator() const {
    if (!_insts.empty() && _insts.back()->is_terminator()) {
      return _insts.back();
    } else {
      return nullptr;
    }
  }

  std::vector<Block*> Block::successors() const {
    if (Inst* terminator = this->terminator()) {
      return terminator->successor_blocks();
    } else {
      return {};
    }
  }

  class Section {
  private:
    Allocator& _allocator;
    std::vector<Block*> _blocks;
    std::vector<Type> _inputs;
    std::vector<Type> _outputs;
    size_t _name_count = 0;
  public:
    Section(Allocator& allocator): _allocator(allocator) {}

    Allocator& allocator() const { return _allocator; }

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

    bool verify(std::ostream& errors) const {
      std::map<Block*, std::set<Block*>> incoming;
      for (Block* block : *this) {
        if (!block->terminator()) {
          errors << "Block ";
          block->write_arg(errors);
          errors << " has no terminator\n";
          return true;
        }

        for (Block* succ : block->successors()) {
          incoming[succ].insert(block);
        }
      }

      std::set<Value*> defined;
      for (Block* block : *this) {
        for (Inst* inst : *block) {
          for (Value* arg : inst->args()) {
            if (arg == nullptr) {
              errors << "Instruction ";
              inst->write_arg(errors);
              errors << " has null argument\n";
              return true;
            }

            if (defined.find(arg) == defined.end()) {
              errors << "Instruction ";
              inst->write_arg(errors);
              errors << " uses undefined value ";
              arg->write_arg(errors);
              errors << "\n";
              return true;
            }
          }

          if (dynmatch(PhiInst, phi, inst)) {
            if (phi->verify(incoming[block], errors)) {
              return true;
            }
          }

          defined.insert(inst);
        }
      }
      return false;
    }
  };

  class Builder {
  private:
    Section* _section = nullptr;
    Block* _block = nullptr;
  public:
    Builder(Section* section): _section(section) {}

    Section* section() const { return _section; }
    Block* block() const { return _block; }

    void move_to_end(Block* block) { _block = block; }
    void insert(Inst* inst) { _block->add(inst); }

    Block* build_block() {
      Block* block = (Block*) _section->allocator().alloc(
        sizeof(Block), alignof(Block)
      );
      new (block) Block();
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

    PhiInst* build_phi(Type type, size_t incoming_count) {
      PhiInst* phi = (PhiInst*) _section->allocator().alloc(
        sizeof(PhiInst) +
        sizeof(Value*) * incoming_count +
        sizeof(Block*) * incoming_count,
        alignof(PhiInst)
      );
      new (phi) PhiInst(type, incoming_count);
      insert(phi);
      return phi;
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

    Value* fold_mod_s(Value* a, Value* b) {
      return build_mod_s(a, b);
    }

    Value* fold_mod_u(Value* a, Value* b) {
      return build_mod_u(a, b);
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
      } else if (true_value == false_value) {
        return true_value;
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

    Value* fold_add_ptr(Value* ptr, Value* index, size_t stride) {
      Value* offset = index;
      if (stride != 1) {
        offset = fold_mul(index, build_const(Type::Int64, stride));
      }
      return fold_add_ptr(ptr, offset);
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
    
    std::unordered_map<AliasingGroup, std::unordered_set<LoadInst*>> _valid_loads;
    std::unordered_map<AliasingGroup, LoadInst*> _exact_loads;

    std::unordered_map<AliasingGroup, GroupState> _memory;
    std::unordered_map<AliasingGroup, Value*> _exact_memory;

    std::unordered_map<size_t, InputInst*> _inputs;

    Pointer pointer(Value* value) {
      auto it = _pointers.find(value);
      if (it == _pointers.end()) {
        return Pointer(value, 0);
      } else {
        return it->second;
      }
    }

    bool could_alias(LoadInst* load, Value* ptr, Type type, AliasingGroup aliasing) {
      if (load->aliasing() != aliasing) {
        return false;
      }

      if (aliasing < 0) {
        return true; // Exact aliasing
      }

      Pointer load_ptr = pointer(load->ptr());
      Pointer store_ptr = pointer(ptr);

      if (load_ptr.base != store_ptr.base) {
        return true;
      }

      Interval load_interval(load_ptr.offset, load->type());
      Interval store_interval(store_ptr.offset, type);

      return load_interval.intersects(store_interval);
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

    Value* build_load(Value* ptr, Type type, LoadFlags flags, AliasingGroup aliasing) {
      if (aliasing < 0) {
        if (Value* value = _exact_memory[aliasing]) {
          return value;
        }
        if (LoadInst* load = _exact_loads[aliasing]) {
          return load;
        }

        LoadInst* load = Builder::build_load(ptr, type, flags, aliasing);
        _exact_loads[aliasing] = load;
        _exact_memory[aliasing] = load;
        return load;
      } else {
        if (Value* value = _memory[aliasing].load(pointer(ptr), type)) {
          return value;
        }

        LoadInst* load = Builder::build_load(ptr, type, flags, aliasing);
        _valid_loads[aliasing].insert(load);
        return load;
      }
    }

    Value* build_store(Value* ptr, Value* value, AliasingGroup aliasing) {
      if (aliasing < 0) {
        if (_exact_memory[aliasing] == value) {
          return nullptr;
        }
        _exact_memory[aliasing] = value;
        _exact_loads.erase(aliasing);
        return Builder::build_store(ptr, value, aliasing);
      } else {
        _memory[aliasing].store(pointer(ptr), value);

        bool noop_store = false;
        for (auto it = _valid_loads[aliasing].begin(); it != _valid_loads[aliasing].end(); ) {
          LoadInst* load = *it;
          if (load == value && pointer(load->ptr()) == pointer(ptr)) {
            noop_store = true;
            it++;
            continue;
          }
          if (could_alias(load, ptr, value->type(), aliasing)) {
            it = _valid_loads[aliasing].erase(it);
          } else {
            it++;
          }
        }

        if (noop_store) {
          return nullptr;
        }

        return Builder::build_store(ptr, value, aliasing);
      }
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

    const T& at(Value* value) const {
      assert(value->name() < _size);
      return _data[value->name()];
    }

    const T& operator[](Value* value) const { return at(value); }
  };

  template <class Self>
  class Pass {
  private:
    std::set<Inst*> _removed;
  public:
    Pass(Section* section) {
      section->autoname();
    }

    ~Pass() {
      for (Inst* inst : _removed) {
        delete inst;
      }
    }

    template <class... Args>
    static void run(Section* section, Args... args) {
      Self self(section, args...);
    }

    void remove(Inst* inst) {
      //_removed.insert(inst);
    }
  };

  class DeadCodeElim: public Pass<DeadCodeElim> {
  public:
    DeadCodeElim(Section* section): Pass(section) {
      ValueMap<bool> used(section);

      for (size_t block_id = section->size(); block_id-- > 0; ) {
        Block* block = (*section)[block_id];
        for (size_t inst_id = block->size(); inst_id-- > 0; ) {
          Inst* inst = block->at(inst_id);
          if (inst->has_side_effect() || inst->is_terminator() || used[inst]) {
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
            remove(inst);
          }
        }
      }
    }
  };

  class PointerAnalysis {
  private:
    ValueMap<Pointer> _pointers;
  public:
    PointerAnalysis(Section* section): _pointers(section) {
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
    }
    
    Pointer at(Value* value) const { return _pointers.at(value); }
    Pointer operator[](Value* value) const { return at(value); }
  };

  class DeadStoreElim: public Pass<DeadStoreElim> {
  private:
    bool could_alias(LoadInst* load, StoreInst* store) {
      if (load->aliasing() != store->aliasing()) {
        return false;
      }

      if (load->aliasing() < 0) {
        return true; // Exact aliasing
      }

      Pointer load_ptr = _pointer_analysis[load->ptr()];
      Pointer store_ptr = _pointer_analysis[store->ptr()];

      if (load_ptr.base != store_ptr.base) {
        return true;
      }

      Interval load_interval(load_ptr.offset, load->type());
      Interval store_interval(store_ptr.offset, store->value()->type());

      return load_interval.intersects(store_interval);
    }

    PointerAnalysis _pointer_analysis;
  public:
    DeadStoreElim(Section* section): Pass(section), _pointer_analysis(section) {
      for (Block* block : *section) {
        std::set<Inst*> unused;
        std::map<Pointer, StoreInst*> last_store;

        std::vector<Inst*> insts = block->move_insts();

        for (Inst* inst : insts) {
          if (dynmatch(StoreInst, store, inst)) {
            Pointer pointer = _pointer_analysis[store->ptr()];
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
            remove(inst);
          }
        }
      }
    }
  };

  class KnownBits {
  public:
    struct Bits {
      Type type = Type::Void;
      uint64_t mask = 0;
      uint64_t value = 0;
      
      Bits() {}
      Bits(Type _type, uint64_t _mask, uint64_t _value):
        type(_type),
        mask(_mask & type_mask(_type)),
        value(_value & type_mask(_type)) {}

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
    KnownBits(Section* section): _section(section), _values(section) {
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

    Bits at(Value* value) const {
      return _values.at(value);
    }

    void write(std::ostream& stream) {
      InfoWriter info_writer([&](std::ostream& stream, Value* value){
        _values[value].write(stream);
      });
      _section->write(stream, &info_writer);
    }
  };

  class UsedBits {
  public:
    struct Bits {
      Type type = Type::Void;
      uint64_t used = 0;

      Bits() {}
      Bits(Type _type, uint64_t _used):
        type(_type), used(_used & type_mask(_type)) {}
      
      static Bits all(Type type) {
        return Bits(type, type_mask(type));
      }

      bool at(size_t bit) const {
        return (used & (uint64_t(1) << bit)) != 0;
      }

      void write(std::ostream& stream) const {
        size_t bits = type == Type::Bool ? 1 : type_size(type) * 8;
        for (size_t it = bits; it-- > 0; ) {
          stream << (at(it) ? 'U' : '_');
        }
      }
    };
  private:
    Section* _section;
    ValueMap<Bits> _values;

    void use(Value* value, uint64_t used) {
      if (_values[value].type != value->type()) {
        assert(_values[value].type == Type::Void);
        _values[value] = Bits(value->type(), 0);
      }
      _values[value].used |= used & type_mask(value->type());
    }

    void use(Value* value, const Bits& used) {
      use(value, used.used);
    }
  public:
    UsedBits(Section* section): _section(section), _values(section) {
      for (size_t block_id = section->size(); block_id-- > 0; ) {
        Block* block = (*section)[block_id];
        for (size_t inst_id = block->size(); inst_id-- > 0; ) {
          Inst* inst = block->at(inst_id);

          if (_values[inst].type != inst->type()) {
            assert(_values[inst].type == Type::Void);
            _values[inst] = Bits(inst->type(), 0);
          }
          
          if (dynmatch(ResizeUInst, resize_u, inst)) {
            use(resize_u->arg(0), _values[inst].used);
          } else if (dynmatch(AndInst, and_inst, inst)) {
            if (dynmatch(ConstInst, const_b, and_inst->arg(1))) {
              use(and_inst->arg(0), _values[inst].used & const_b->value());
            } else {
              use(and_inst->arg(0), _values[inst]);
            }
            use(and_inst->arg(1), _values[inst]);
          } else if (dynamic_cast<OrInst*>(inst) ||
                     dynamic_cast<XorInst*>(inst)) {
            // Element-wise instructions
            for (Value* arg : inst->args()) {
              use(arg, _values[inst]);
            }
          } else if (dynmatch(SelectInst, select, inst)) {
            if (_values[inst].used != 0) {
              use(select->cond(), Bits::all(select->cond()->type()));
            }
            use(select->arg(1), _values[inst]);
            use(select->arg(2), _values[inst]);
          } else {
            if (inst->has_side_effect() || inst->is_terminator() || _values[inst].used != 0) {
              for (Value* arg : inst->args()) {
                use(arg, Bits::all(arg->type()));
              }
            } else {
              for (Value* arg : inst->args()) {
                use(arg, 0);
              }
            }
          }
        }
      }
    }

    Bits at(Value* value) const {
      return _values.at(value);
    }

    void write(std::ostream& stream) {
      InfoWriter info_writer([&](std::ostream& stream, Value* value){
        _values[value].write(stream);
      });
      _section->write(stream, &info_writer);
    }
  };

  class Simplify: public Pass<Simplify> {
  public:
    Simplify(Section* section): Pass(section) {
      KnownBits known_bits(section);
      UsedBits used_bits(section);

      ValueMap<Value*> substs(section);

      for (Block* block : *section) {
        std::vector<Inst*> insts = block->move_insts();
        for (Inst* inst : insts) {
          for (size_t it = 0; it < inst->arg_count(); it++) {
            Value* arg = inst->arg(it);
            if (substs[arg]) {
              inst->set_arg(it, substs[arg]);
            }
          }

          if (dynmatch(AndInst, and_inst, inst)) {
            KnownBits::Bits a = known_bits.at(and_inst->arg(0));
            KnownBits::Bits b = known_bits.at(and_inst->arg(1));
            UsedBits::Bits used = used_bits.at(inst);

            if (b.is_const() && ((b.value ^ type_mask(b.type)) & (~a.mask | a.value)) == 0) {
              substs[inst] = and_inst->arg(0);
              remove(inst);
              continue;
            } else if (b.is_const() && (used.used & ~b.value) == 0) {
              substs[inst] = and_inst->arg(0);
              remove(inst);
              continue;
            }
          }

          block->add(inst);
        }
      }
    }
  };
}

