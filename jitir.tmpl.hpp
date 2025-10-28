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
#include <set>
#include <map>
#include <optional>

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

    bool operator==(const Self& other) const {
      return _flags == other._flags;
    }

    bool operator!=(const Self& other) const {
      return !(*this == other);
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

template <>
struct std::hash<metajit::InputFlags> {
  size_t operator()(const metajit::InputFlags& flags) const {
    return std::hash<uint32_t>()((uint32_t) flags);
  }
};

std::ostream& operator<<(std::ostream& stream, metajit::InputFlags flags) {
  flags.write(stream);
  return stream;
}

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

template <>
struct std::hash<metajit::Type> {
  size_t operator()(const metajit::Type& type) const {
    return std::hash<size_t>()((size_t) type);
  }
};

namespace metajit {
  class Value {
  private:
    Type _type;
  public:
    Value(Type type): _type(type) {}
    virtual ~Value() {}

    Type type() const { return _type; }

    virtual void write_arg(std::ostream& stream) const = 0;

    virtual bool equals(const Value* other) const = 0;
    virtual size_t hash() const = 0;
  };

  class Const final: public Value {
  private:
    uint64_t _value = 0;
  public:
    Const(Type type, uint64_t value): Value(type), _value(value) {}
    uint64_t value() const { return _value; }

    void write_arg(std::ostream& stream) const override {
      stream << _value;
    }

    bool equals(const Value* other) const override {
      if (typeid(*this) != typeid(*other)) {
        return false;
      }

      const Const* const_other = (const Const*) other;
      return type() == const_other->type() &&
             value() == const_other->value();
    }

    size_t hash() const override {
      return std::hash<uint64_t>()(_value) ^ std::hash<Type>()(type());
    }
  };

  class Input final: public Value {
  private:
    size_t _index = 0;
    InputFlags _flags;
  public:
    Input(Type type, size_t index, InputFlags flags):
      Value(type), _index(index), _flags(flags) {}

    size_t index() const { return _index; }
    InputFlags flags() const { return _flags; }

    void write_arg(std::ostream& stream) const override {
      stream << "%input" << _index;
    }

    bool equals(const Value* other) const override {
      if (typeid(*this) != typeid(*other)) {
        return false;
      }

      const Input* input_other = (const Input*) other;
      return type() == input_other->type() &&
             index() == input_other->index();
    }

    size_t hash() const override {
      return std::hash<size_t>()(_index);
    }
  };

  class Context {
  private:
    Allocator _const_allocator;
  public:
    Context() {}

    Const* build_const(Type type, uint64_t value) {
      Const* constant = (Const*) _const_allocator.alloc(sizeof(Const), alignof(Const));
      new (constant) Const(type, value);
      return constant;
    }
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

  template <class T>
  class LinkedListItem {
  private:
    T* _prev = nullptr;
    T* _next = nullptr;
  public:
    LinkedListItem() {}

    inline T* prev() const { return _prev; }
    inline void set_prev(T* prev) { _prev = prev; }

    inline T* next() const { return _next; }
    inline void set_next(T* next) { _next = next; }
  };

  template <class T>
  class Range {
  private:
    T _begin;
    T _end;
  public:
    Range(const T& begin, const T& end): _begin(begin), _end(end) {}

    T begin() const { return _begin; }
    T end() const { return _end; }
  };

  template <class T>
  class LinkedList {
  private:
    T* _first = nullptr;
    T* _last = nullptr;
  public:
    LinkedList() {}

    T* first() const { return _first; }
    T* last() const { return _last; }

    bool empty() const { return _first == nullptr; }

    void add(T* item) {
      assert(!item->prev() && !item->next());
      item->set_prev(_last);
      if (_last) {
        _last->set_next(item);
      } else {
        _first = item;
      }
      _last = item;
    }

    void insert_before(T* before, T* item) {
      assert(!item->prev() && !item->next());
      if (before == nullptr) {
        add(item);
      } else {
        item->set_next(before);
        item->set_prev(before->prev());
        if (before->prev()) {
          before->prev()->set_next(item);
        } else {
          _first = item;
        }
        before->set_prev(item);
      }
    }

    void remove(T* item) {
      if (item->prev()) {
        item->prev()->set_next(item->next());
      } else {
        _first = item->next();
      }
      if (item->next()) {
        item->next()->set_prev(item->prev());
      } else {
        _last = item->prev();
      }
      item->set_prev(nullptr);
      item->set_next(nullptr);
    }

    class iterator {
    private:
      LinkedList* _list;
      T* _item;
    public:
      iterator(LinkedList* list, T* item): _list(list), _item(item) {}
      
      T* operator*() const { return _item; }
      
      iterator& operator++() { 
        _item = _item->next(); 
        return *this;
      }

      iterator operator++(int) { 
        iterator iter = *this;
        ++(*this);
        return iter;
      }

      bool operator==(const iterator& other) const { return _item == other._item; }
      bool operator!=(const iterator& other) const { return !(*this == other); }

      iterator erase() {
        T* next = _item->next();
        _list->remove(_item);
        return iterator(_list, next);
      }
    };

    iterator begin() { return iterator(this, _first); }
    iterator end() { return iterator(this, nullptr); }
    
    class reverse_iterator {
    private:
      LinkedList* _list;
      T* _item;
    public:
      reverse_iterator(LinkedList* list, T* item): _list(list), _item(item) {}

      T* operator*() const { return _item; }

      reverse_iterator& operator++() { 
        _item = _item->prev(); 
        return *this;
      }

      reverse_iterator operator++(int) { 
        reverse_iterator iter = *this;
        ++(*this);
        return iter;
      }

      bool operator==(const reverse_iterator& other) const { return _item == other._item; }
      bool operator!=(const reverse_iterator& other) const { return !(*this == other); }
      
      reverse_iterator erase() {
        T* prev = _item->prev();
        _list->remove(_item);
        return reverse_iterator(_list, prev);
      }
    };

    reverse_iterator rbegin() { return reverse_iterator(this, _last); }
    reverse_iterator rend() { return reverse_iterator(this, nullptr); }

    Range<iterator> range() { return Range<iterator>(begin(), end()); }
    Range<reverse_iterator> rev_range() { return Range<reverse_iterator>(rbegin(), rend()); }
  };

  class Inst: public Value, public LinkedListItem<Inst> {
  private:
    Span<Value*> _args;
    size_t _name = 0;
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

    size_t name() const { return _name; }
    void set_name(size_t name) { _name = name; }

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

    void write_arg(std::ostream& stream) const override {
      stream << '%' << _name;
    }

    bool has_side_effect() const;
    bool is_terminator() const;
    std::vector<Block*> successor_blocks() const;
  };

  struct InfoWriter {
  public:
    using InstFn = std::function<void(std::ostream&, Inst*)>;

    InstFn inst = nullptr;

    InfoWriter() {}
    InfoWriter(const InstFn& _inst): inst(_inst) {}
  };

  class Block {
  private:
    LinkedList<Inst> _insts;
    size_t _name = 0;
  public:
    Block() {}

    auto begin() { return _insts.begin(); }
    auto end() { return _insts.end(); }

    auto rbegin() { return _insts.rbegin(); }
    auto rend() { return _insts.rend(); }

    auto range() { return _insts.range(); }
    auto rev_range() { return _insts.rev_range(); }

    bool empty() const { return _insts.empty(); }

    size_t name() const { return _name; }
    void set_name(size_t name) { _name = name; }

    void add(Inst* inst) {
      _insts.add(inst);
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
        if (info_writer && info_writer->inst) {
          stream << " ; ";
          info_writer->inst(stream, inst);
        }
        stream << '\n';
      }
    }

    void write_arg(std::ostream& stream) {
      stream << 'b' << _name;
    }

    template <class Fn>
    void filter_inplace(Fn fn) {
      for (auto it = begin(); it != end(); ) {
        Inst* inst = *it;
        if (fn(inst)) {
          it++;
        } else {
          it = it.erase();
        }
      }
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
}

template <>
struct std::hash<metajit::LoadFlags> {
  size_t operator()(const metajit::LoadFlags& flags) const {
    return std::hash<uint32_t>()((uint32_t) flags);
  }
};

std::ostream& operator<<(std::ostream& stream, metajit::LoadFlags flags) {
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

    bool equals(const Value* other) const override {
      if (typeid(*this) != typeid(*other)) {
        return false;
      }

      const PhiInst* phi_other = (const PhiInst*) other;
      if (type() != phi_other->type() ||
          arg_count() != phi_other->arg_count()) {
        return false;
      }

      for (size_t it = 0; it < arg_count(); it++) {
        if (arg(it) != phi_other->arg(it) || 
            block(it) != phi_other->block(it)) {
          return false;
        }
      }

      return true;
    }

    size_t hash() const override {
      size_t hash = 2345678; // Some random seed
      hash ^= std::hash<Type>()(type());
      hash ^= std::hash<size_t>()(arg_count());

      for (size_t it = 0; it < arg_count(); it++) {
        hash ^= std::hash<Value*>()(arg(it));
        hash ^= std::hash<Block*>()(block(it));
      }

      return hash;
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
    return dynamic_cast<const StoreInst*>(this);
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
    if (!_insts.empty() && _insts.last()->is_terminator()) {
      return _insts.last();
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
    Context& _context;
    Allocator& _allocator;
    std::vector<Block*> _blocks;
    std::vector<Input*> _inputs;
    size_t _name_count = 0;
  public:
    Section(Context& context, Allocator& allocator):
      _context(context), _allocator(allocator) {}

    Context& context() const { return _context; }
    Allocator& allocator() const { return _allocator; }

    auto begin() const { return _blocks.begin(); }
    auto end() const { return _blocks.end(); }

    size_t size() const { return _blocks.size(); }
    Block* operator[](size_t index) const { return _blocks.at(index); }

    Block* entry() const { return _blocks.at(0); }

    const std::vector<Input*>& inputs() const { return _inputs; }

    size_t name_count() const { return _name_count; }

    Input* add_input(Type type, InputFlags flags = InputFlags()) {
      Input* input = (Input*) _allocator.alloc(sizeof(Input), alignof(Input));
      new (input) Input(type, _inputs.size(), flags);
      _inputs.push_back(input);
      return input;
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
      for (Input* input : _inputs) {
        if (is_first) {
          is_first = false;
        } else {
          stream << ", ";
        }
        stream << input->type() << " ";
        input->write_arg(stream);
        stream << " " << input->flags();
      }
      stream << ") {\n";
      for (Block* block : _blocks) {
        block->write(stream, info_writer);
      }
      stream << "}\n";
    }

    bool verify(std::ostream& errors) {
      autoname();

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

      for (Input* input : _inputs) {
        defined.insert(input);
      }

      for (Block* block : *this) {
        for (Inst* inst : *block) {
          for (Value* arg : inst->args()) {
            if (arg == nullptr) {
              errors << "Instruction ";
              inst->write_arg(errors);
              errors << " has null argument\n";
              return true;
            }

            if (dynamic_cast<const Inst*>(arg) &&
                defined.find(arg) == defined.end()) {
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

    Const* build_const(Type type, uint64_t value) {
      return _section->context().build_const(type, value);
    }

    Const* build_const_fast(Type type, uint64_t value) {
      Const* constant = (Const*) _section->allocator().alloc(sizeof(Const), alignof(Const));
      new (constant) Const(type, value);
      return constant;
    }

    ${builder}

    Input* build_input(Type type, InputFlags flags = InputFlags()) {
      return _section->add_input(type, flags);
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
      if (dynamic_cast<Const*>(a)) {
        std::swap(a, b);
      }

      if (dynmatch(Const, const_b, b)) {
        if (const_b->value() == 0) {
          return a;
        }
      }

      return build_add(a, b);
    }

    Value* fold_sub(Value* a, Value* b) {
      if (dynmatch(Const, const_b, b)) {
        if (const_b->value() == 0) {
          return a;
        }
      }

      return build_sub(a, b);
    }

    Value* fold_mul(Value* a, Value* b) {
      if (dynamic_cast<Const*>(a)) {
        std::swap(a, b);
      }

      if (dynmatch(Const, const_b, b)) {
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
      if (dynamic_cast<Const*>(a)) {
        std::swap(a, b);
      }

      if (dynmatch(Const, const_b, b)) {
        if (dynmatch(Const, const_a, a)) {
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
      if (dynamic_cast<Const*>(a)) {
        std::swap(a, b);
      }

      if (dynmatch(Const, constant, b)) {
        if (constant->value() == 0) {
          return a;
        } else if (constant->value() == type_mask(a->type())) {
          return constant;
        }
      }
      return build_or(a, b);
    }

    Value* fold_xor(Value* a, Value* b) {
      if (dynamic_cast<Const*>(a)) {
        std::swap(a, b);
      }

      if (dynmatch(Const, constant, b)) {
        if (constant->value() == 0) {
          return a;
        }
      }
      return build_xor(a, b);
    }

    Value* fold_eq(Value* a, Value* b) {
      if (dynamic_cast<Const*>(a)) {
        std::swap(a, b);
      }

      if (a == b) {
        return build_const(Type::Bool, 1);
      }

      return build_eq(a, b);
    }

    Value* fold_lt_s(Value* a, Value* b) {
      return build_lt_s(a, b);
    }

    Value* fold_lt_u(Value* a, Value* b) {
      return build_lt_u(a, b);
    }

    Value* fold_select(Value* cond, Value* true_value, Value* false_value) {
      if (dynmatch(Const, const_cond, cond)) {
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
      if (dynmatch(Const, constant, offset)) {
        if (constant->value() == 0) {
          return ptr;
        } else if (dynmatch(AddPtrInst, add_ptr, ptr)) {
          if (dynmatch(Const, inner_const, add_ptr->offset())) {
            return build_add_ptr(
              add_ptr->ptr(),
              build_const(Type::Int64, inner_const->value() + constant->value())
            );
          }
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
      } else if (dynmatch(Const, constant, a)) {
        uint64_t value = constant->value() & type_mask(type);
        return build_const(type, value);
      }
      return build_resize_u(a, type);
    }

    Value* fold_shl(Value* a, Value* b) {
      if (dynmatch(Const, const_b, b)) {
        if (dynmatch(Const, const_a, a)) {
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
      if (dynmatch(Const, constant, b)) {
        if (constant->value() == 0) {
          return a;
        }
      }
      return build_shr_u(a, b);
    }

    Value* fold_shr_s(Value* a, Value* b) {
      if (dynmatch(Const, constant, b)) {
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

  template <class T>
  class ExpandingVector {
  private:
    std::vector<T> _data;
  public:
    ExpandingVector() {}

    T& operator[](size_t index) {
      if (index >= _data.size()) {
        _data.resize(index + 1);
      }
      return _data[index];
    }
  };

  class TraceBuilder: public Builder {
  private:
    // We perform optimizations during trace generation
    
    struct GroupState {
      Value* base = nullptr;
      std::map<uint64_t, Value*> stores;

      GroupState() {}

      Value* load(Value* _base, uint64_t offset, Type type) {
        if (_base == base &&
            _base != nullptr &&
            base != nullptr &&
            stores.find(offset) != stores.end() &&
            stores.at(offset)->type() == type) {
          return stores.at(offset);
        }
        return nullptr;
      }

      void store(Value* _base, uint64_t offset, Value* value) {
        if (_base == nullptr) {
          base = nullptr;
          stores.clear();
          return;
        }

        if (base != _base) {
          base = _base;
          stores.clear();
        }
        // TODO: Overlapping
        stores[offset] = value;
      }
    };
    
    std::unordered_map<AliasingGroup, std::unordered_set<LoadInst*>> _valid_loads;
    ExpandingVector<LoadInst*> _exact_loads;

    std::unordered_map<AliasingGroup, GroupState> _memory;
    ExpandingVector<Value*> _exact_memory;

    bool could_alias(LoadInst* load, Value* ptr, Type type, AliasingGroup aliasing, uint64_t offset) {
      if (load->aliasing() != aliasing) {
        return false;
      }

      if (aliasing < 0) {
        return true; // Exact aliasing
      }

      if (load->ptr() != ptr) {
        return true;
      }

      Interval load_interval(load->offset(), load->type());
      Interval store_interval(offset, type);

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

    Value* build_add_ptr(Value* ptr, Value* offset) {
      return Builder::fold_add_ptr(ptr, offset);
    }

    // We override load/store to do simple load/store forwarding

    Value* build_load(Value* ptr, Type type, LoadFlags flags, AliasingGroup aliasing, uint64_t offset) {
      if (dynmatch(AddPtrInst, add_ptr, ptr)) {
        if (dynmatch(Const, const_offset, add_ptr->offset())) {
          ptr = add_ptr->ptr();
          offset += const_offset->value();
        }
      }

      if (aliasing < 0) {
        if (Value* value = _exact_memory[-aliasing]) {
          return value;
        }
        if (LoadInst* load = _exact_loads[-aliasing]) {
          return load;
        }

        LoadInst* load = Builder::build_load(ptr, type, flags, aliasing, offset);
        _exact_loads[-aliasing] = load;
        _exact_memory[-aliasing] = load;
        return load;
      } else {
        if (Value* value = _memory[aliasing].load(ptr, offset, type)) {
          return value;
        }

        LoadInst* load = Builder::build_load(ptr, type, flags, aliasing, offset);
        _valid_loads[aliasing].insert(load);
        return load;
      }
    }

    Value* build_store(Value* ptr, Value* value, AliasingGroup aliasing, uint64_t offset) {
      if (dynmatch(AddPtrInst, add_ptr, ptr)) {
        if (dynmatch(Const, const_offset, add_ptr->offset())) {
          ptr = add_ptr->ptr();
          offset += const_offset->value();
        }
      }

      if (aliasing < 0) {
        if (_exact_memory[-aliasing] == value) {
          return nullptr;
        }
        _exact_memory[-aliasing] = value;
        _exact_loads[-aliasing] = nullptr;
        return Builder::build_store(ptr, value, aliasing, offset);
      } else {
        _memory[aliasing].store(ptr, offset, value);

        bool noop_store = false;
        for (auto it = _valid_loads[aliasing].begin(); it != _valid_loads[aliasing].end(); ) {
          LoadInst* load = *it;
          if (load == value && load->ptr() == ptr && load->offset() == offset) {
            noop_store = true;
            it++;
            continue;
          }
          if (could_alias(load, ptr, value->type(), aliasing, offset)) {
            it = _valid_loads[aliasing].erase(it);
          } else {
            it++;
          }
        }

        if (noop_store) {
          return nullptr;
        }

        return Builder::build_store(ptr, value, aliasing, offset);
      }
    }

  };

  ${capi}

  template<class T>
  class InstMap {
  private:
    T* _data = nullptr;
    size_t _size = 0;
  public:
    InstMap() {}
    InstMap(Section* section) { init(section); }

    InstMap(const InstMap<T>&) = delete;
    InstMap<T>& operator=(const InstMap<T>&) = delete;

    ~InstMap() { if (_data) { delete[] _data; } }

    void init(Section* section) {
      assert(_data == nullptr);
      _size = section->name_count();
      _data = new T[_size]();
    }

    T& at(Inst* inst) {
      assert(inst->name() < _size);
      return _data[inst->name()];
    }

    T& operator[](Inst* inst) { return at(inst); }

    const T& at(Inst* inst) const {
      assert(inst->name() < _size);
      return _data[inst->name()];
    }

    const T& operator[](Inst* inst) const { return at(inst); }
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
      InstMap<bool> used(section);

      for (size_t block_id = section->size(); block_id-- > 0; ) {
        Block* block = (*section)[block_id];
        for (Inst* inst : block->rev_range()) {
          if (inst->has_side_effect() || inst->is_terminator() || used[inst]) {
            used[inst] = true;
            for (Value* arg : inst->args()) {
              if (dynmatch(Inst, inst, arg)) {
                used[inst] = true;
              }
            }
          }
        }
      }

      for (Block* block : *section) {
        block->filter_inplace([&](Inst* inst) {
          return used[inst];
        });
      }
    }
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

      if (load->ptr() != store->ptr()) {
        return true;
      }

      Interval load_interval(load->offset(), load->type());
      Interval store_interval(store->offset(), store->value()->type());

      return load_interval.intersects(store_interval);
    }
  public:
    DeadStoreElim(Section* section): Pass(section) {
      for (Block* block : *section) {
        std::set<Inst*> unused;
        std::map<Pointer, StoreInst*> last_store;

        for (Inst* inst : *block) {
          if (dynmatch(StoreInst, store, inst)) {
            Pointer pointer(store->ptr(), store->offset());
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

        block->filter_inplace([&](Inst* inst) {
          return unused.find(inst) == unused.end();
        });
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
    InstMap<Bits> _values;
  public:
    KnownBits(Section* section): _section(section), _values(section) {
      for (Block* block : *section) {
        for (Inst* inst : *block) {
          if (dynmatch(ResizeUInst, resize_u, inst)) {
            Bits a = at(resize_u->arg(0));
            _values[inst] = a.resize_u(resize_u->type());
          }

          #define binop(name, expr) \
            else if (dynmatch(name, binop, inst)) { \
              Bits a = at(binop->arg(0)); \
              Bits b = at(binop->arg(1)); \
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
      if (dynmatch(Const, constant, value)) {
        return Bits(
          constant->type(),
          type_mask(constant->type()),
          constant->value()
        );
      } else if (dynmatch(Input, input, value)) {
        return Bits(input->type(), 0, 0);
      } else if (dynmatch(Inst, inst, value)) {
        return _values.at(inst);
      } else {
        assert(false); // Unreachable
        return Bits();
      }
    }

    void write(std::ostream& stream) {
      InfoWriter info_writer([&](std::ostream& stream, Inst* inst){
        _values[inst].write(stream);
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
    InstMap<Bits> _values;

    void use(Value* value, uint64_t used) {
      if (dynmatch(Inst, inst, value)) {
        if (_values[inst].type != value->type()) {
          assert(_values[inst].type == Type::Void);
          _values[inst] = Bits(value->type(), 0);
        }
        _values[inst].used |= used & type_mask(value->type());
      }
    }

    void use(Value* value, const Bits& used) {
      use(value, used.used);
    }
  public:
    UsedBits(Section* section): _section(section), _values(section) {
      for (size_t block_id = section->size(); block_id-- > 0; ) {
        Block* block = (*section)[block_id];
        for (Inst* inst : block->rev_range()) {
          if (_values[inst].type != inst->type()) {
            assert(_values[inst].type == Type::Void);
            _values[inst] = Bits(inst->type(), 0);
          }
          
          if (dynmatch(ResizeUInst, resize_u, inst)) {
            use(resize_u->arg(0), _values[inst].used);
          } else if (dynmatch(AndInst, and_inst, inst)) {
            if (dynmatch(Const, const_b, and_inst->arg(1))) {
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
      if (dynmatch(Inst, inst, value)) {
        return _values.at(inst);
      } else {
        assert(false); // Unreachable
        return Bits();
      }
    }

    void write(std::ostream& stream) {
      InfoWriter info_writer([&](std::ostream& stream, Inst* inst){
        _values[inst].write(stream);
      });
      _section->write(stream, &info_writer);
    }
  };

  class Simplify: public Pass<Simplify> {
  public:
    Simplify(Section* section): Pass(section) {
      KnownBits known_bits(section);
      UsedBits used_bits(section);

      InstMap<Value*> substs(section);

      for (Block* block : *section) {
        for (auto inst_it = block->begin(); inst_it != block->end(); ) {
          Inst* inst = *inst_it;

          for (size_t it = 0; it < inst->arg_count(); it++) {
            Value* arg = inst->arg(it);
            if (dynmatch(Inst, inst_arg, arg)) {
              if (substs[inst_arg]) {
                inst->set_arg(it, substs[inst_arg]);
              }
            }
          }

          if (dynmatch(AndInst, and_inst, inst)) {
            KnownBits::Bits a = known_bits.at(and_inst->arg(0));
            KnownBits::Bits b = known_bits.at(and_inst->arg(1));
            UsedBits::Bits used = used_bits.at(inst);

            if (b.is_const() && ((b.value ^ type_mask(b.type)) & (~a.mask | a.value)) == 0) {
              substs[inst] = and_inst->arg(0);
              inst_it = inst_it.erase();
              remove(inst);
              continue;
            } else if (b.is_const() && (used.used & ~b.value) == 0) {
              substs[inst] = and_inst->arg(0);
              inst_it = inst_it.erase();
              remove(inst);
              continue;
            }
          }

          inst_it++;
        }
      }
    }
  };

  class CommonSubexprElim: public Pass<CommonSubexprElim> {
  private:
    struct Lookup {
      Value* value = nullptr;

      Lookup(Value* _value): value(_value) {}

      bool operator==(const Lookup& other) const {
        return value->equals(other.value);
      }
    };

    struct LookupHash {
      size_t operator()(const Lookup& lookup) const {
        return lookup.value->hash();
      }
    };
  public:
    CommonSubexprElim(Section* section): Pass(section) {
      std::unordered_map<Value*, Value*> substs;
      std::unordered_map<Lookup, Const*, LookupHash> consts;
      for (Block* block : *section) {
        std::unordered_map<Lookup, Value*, LookupHash> canon;

        for (auto inst_it = block->begin(); inst_it != block->end(); ) {
          Inst* inst = *inst_it;

          for (size_t it = 0; it < inst->arg_count(); it++) {
            Value* arg = inst->arg(it);
            if (substs.find(arg) != substs.end()) {
              inst->set_arg(it, substs.at(arg));
            } else if (dynmatch(Const, constant, arg)) {
              Lookup lookup(constant);
              if (consts.find(lookup) != consts.end()) {
                inst->set_arg(it, consts.at(lookup));
                substs[constant] = consts.at(lookup);
              } else {
                consts[lookup] = constant;
              }
            }
          }

          if (inst->has_side_effect() || inst->is_terminator()) {
            inst_it++;
            continue;
          }
          
          Lookup lookup(inst);
          if (canon.find(lookup) == canon.end()) {
            canon[lookup] = inst;
            inst_it++;
          } else {
            substs[inst] = canon.at(lookup);
            inst_it = inst_it.erase();
            remove(inst);
          }
        }
      }
    }
  };

  class Uses {
  public:
    struct Use {
      Inst* inst = nullptr;
      size_t index = 0;

      Use() {}
      Use(Inst* _inst, size_t _index):
        inst(_inst), index(_index) {}
    };
  private:
    Section* _section;
    InstMap<std::vector<Use>> _uses;
  public:
    Uses(Section* section): _section(section), _uses(section) {
      for (Block* block : *section) {
        for (Inst* inst : *block) {
          for (size_t it = 0; it < inst->arg_count(); it++) {
            if (dynmatch(Inst, arg_inst, inst->arg(it))) {
              _uses[arg_inst].emplace_back(inst, it);
            }
          }
        }
      }
    }

    const std::vector<Use>& at(Inst* inst) const {
      return _uses.at(inst);
    }
  };

  class ConstnessAnalysis {
  public:
    static const size_t ALWAYS = 0;
  private:
    Section* _section;
    InstMap<size_t> _groups;
    std::vector<size_t> _inputs;
    size_t _next_group = 1;
  public:
    ConstnessAnalysis(Section* section): _section(section), _groups(section) {
      _inputs.reserve(section->inputs().size());
      for (Input* input : section->inputs()) {
        if (input->flags().has(InputFlags::AssumeConst)) {
          _inputs[input->index()] = ALWAYS;
        } else {
          _inputs[input->index()] = _next_group++;
        }
      }

      for (Block* block : *section) {
        for (Inst* inst : *block) {
          if (dynmatch(FreezeInst, freeze, inst)) {
            _groups[inst] = ALWAYS;
          } else if (dynmatch(LoadInst, load, inst)) {
            if (load->flags().has(LoadFlags::Pure)) {
              _groups[inst] = at(load->ptr());
            } else {
              _groups[inst] = _next_group++;
            }
          } else if (inst->has_side_effect() || inst->is_terminator()) {
            _groups[inst] = _next_group++;
          } else {
            size_t group = ALWAYS;
            for (Value* arg : inst->args()) {
              size_t arg_group = at(arg);
              if (arg_group != ALWAYS && arg_group != group) {
                if (group == ALWAYS) {
                  group = arg_group;
                } else {
                  group = _next_group;
                }
              }
            }

            // Instructions for which we implement short-circuiting
            // inside the generating extension may be const even though
            // not all arguments are const. This means they need to be
            // in a separate group.
            if (dynamic_cast<AndInst*>(inst) ||
                dynamic_cast<OrInst*>(inst) ||
                dynamic_cast<SelectInst*>(inst) ||
                dynamic_cast<LoadInst*>(inst)) {
              if (group != ALWAYS) {
                group = _next_group;
              }
            }

            if (group == _next_group) {
              _next_group++;
            }

            _groups[inst] = group;
          }
        }
      }
    }

    size_t at(Value* value) const {
      if (dynmatch(Const, constant, value)) {
        return ALWAYS;
      } else if (dynmatch(Input, input, value)) {
        return _inputs[input->index()];
      } else if (dynmatch(Inst, inst, value)) {
        return _groups.at(inst);
      } else {
        assert(false); // Unreachable
        return 0;
      }
    }

    void write(std::ostream& stream) {
      InfoWriter info_writer([&](std::ostream& stream, Inst* inst) {
        size_t group = _groups.at(inst);
        if (group == ALWAYS) {
          stream << "always";
        } else {
          stream << group;
        }
      });
      _section->write(stream, &info_writer);
    }
  };

  class TraceCapabilities {
  private:
    Section* _section;
    ConstnessAnalysis& _constness;
    InstMap<bool> _can_trace_inst;
    InstMap<bool> _can_trace_const;

    void used_by(Inst* inst, Inst* by) {
      if (_can_trace_inst.at(by)) {
        if (_constness.at(by) != _constness.at(inst) ||
            (is_int_or_bool(inst->type()) && !is_int_or_bool(by->type()))) {
          _can_trace_const[inst] = true;
        }
        if (_constness.at(inst) != ConstnessAnalysis::ALWAYS) {
          _can_trace_inst[inst] = true;
        }
      }

      // Phis cannot generate new constants, so all arguments need to be const traceable
      if (dynmatch(PhiInst, phi, by)) {
        if (_can_trace_const.at(by)) {
          _can_trace_const[inst] = true;
        }
      }
    }
  public:
    TraceCapabilities(Section* section, ConstnessAnalysis& constness):
        _section(section),
        _constness(constness),
        _can_trace_inst(section),
        _can_trace_const(section) {
    
      for (size_t block_id = section->size(); block_id-- > 0; ) {
        Block* block = (*section)[block_id];
        for (Inst* inst : block->rev_range()) {
          if (inst->has_side_effect() ||
              inst->is_terminator() ||
              dynamic_cast<FreezeInst*>(inst)) {
            _can_trace_inst[inst] = true;
            _can_trace_const[inst] = true;
          }

          for (Value* arg : inst->args()) {
            if (dynmatch(Inst, arg_inst, arg)) {
              used_by(arg_inst, inst);
            }
          }
        }
      }
    }

    bool can_trace_const(Inst* inst) const {
      return _can_trace_const[inst];
    }

    bool can_trace_inst(Inst* inst) const {
      return _can_trace_inst[inst];
    }

    bool any(Inst* inst) const {
      return can_trace_const(inst) || can_trace_inst(inst);
    }

    void write(std::ostream& stream) {
      InfoWriter info_writer([&](std::ostream& stream, Inst* inst) {
        if (can_trace_const(inst)) {
          stream << "trace_const ";
        }
        if (can_trace_inst(inst)) {
          stream << "trace_inst ";
        }
        stream << "group=" << _constness.at(inst);
      });
      _section->write(stream, &info_writer);
    }
  };
}

