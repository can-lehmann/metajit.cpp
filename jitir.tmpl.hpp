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
#include <cstring>

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

    template <class T>
    T* alloc() {
      return (T*) alloc(sizeof(T), alignof(T));
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

    Self operator|(const Self& other) const {
      return Self(_flags | other._flags);
    }
  };

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

  inline size_t type_width(Type type) {
    if (type == Type::Bool) {
      return 1;
    } else {
      return type_size(type) * 8;
    }
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

    virtual bool is_inst() const { return false; }
    virtual bool is_named() const { return false; }
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

  class Context {
  private:
    Allocator _const_allocator;
  public:
    Context() {}

    Const* build_const(Type type, uint64_t value) {
      Const* constant = (Const*) _const_allocator.alloc(sizeof(Const), alignof(Const));
      new (constant) Const(type, value & type_mask(type));
      return constant;
    }

    const char* alloc_string(const std::string& string) {
      char* data = (char*) _const_allocator.alloc(string.size() + 1, alignof(char));
      std::copy(string.data(), string.data() + string.size(), data);
      data[string.size()] = '\0';
      return data;
    }
  };

  class Block;
  
  template <class T>
  class Span {
  private:
    T* _data = nullptr;
    size_t _size = 0;
  public:
    Span() {}
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

      iterator at(T* new_item) {
        return iterator(_list, new_item);
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

  template<class T>
  class NameMap;

  class NamedValue : public Value {
  private:
    size_t _name = 0;
  public:
    NamedValue(Type type): Value(type) {}

    size_t name() const { return _name; }
    void set_name(size_t name) { _name = name; }

    void write_arg(std::ostream& stream) const override {
      stream << '%' << _name;
    }

    bool is_named() const override {
      return true;
    }
  };

  class Inst: public NamedValue, public LinkedListItem<Inst> {
  private:
    Span<Value*> _args;
  public:
    Inst(Type type, const Span<Value*>& args):
      NamedValue(type), _args(args) {}

    const Span<Value*>& args() const { return _args; }
    void set_args(const Span<Value*>& args) { _args = args; }

    size_t arg_count() const { return _args.size(); }
    Value* arg(size_t index) const { return _args.at(index); }

    void set_arg(size_t index, Value* value) {
      assert(!value || !_args.at(index) || value->type() == _args.at(index)->type());
      _args[index] = value;
    }

    void substitute_args(NameMap<Value*>& substs);

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

    void write_stmt(std::ostream& stream) const {
      if (type() != Type::Void) {
        write_arg(stream);
        stream << " = ";
      }
      write(stream);
    }

    bool has_side_effect() const;
    bool is_terminator() const;
    std::vector<Block*> successor_blocks() const;

    bool is_inst() const override { return true; }
  };

  class Arg: public NamedValue {
  private:
    size_t _index = 0;
  public:
    Arg(Type type, size_t index):
      NamedValue(type), _index(index) {}

    size_t index() const { return _index; }

    bool equals(const Value* other) const override {
      if (typeid(*this) != typeid(*other)) {
        return false;
      }

      const Arg* arg_other = (const Arg*) other;
      return type() == arg_other->type() &&
             index() == arg_other->index();
    }

    size_t hash() const override {
      return std::hash<size_t>()(_index);
    }
  };

  struct InfoWriter {
  public:
    using InstFn = std::function<void(std::ostream&, Inst*)>;

    InstFn inst = nullptr;

    InfoWriter() {}
    InfoWriter(const InstFn& _inst): inst(_inst) {}
  };

  class Block: public LinkedListItem<Block> {
  private:
    Span<Arg*> _args;
    LinkedList<Inst> _insts;
    size_t _name = 0;
  public:
    Block() {}
    Block(const Span<Arg*>& args): _args(args) {}

    auto begin() { return _insts.begin(); }
    auto end() { return _insts.end(); }

    auto rbegin() { return _insts.rbegin(); }
    auto rend() { return _insts.rend(); }

    auto range() { return _insts.range(); }
    auto rev_range() { return _insts.rev_range(); }

    bool empty() const { return _insts.empty(); }

    const Span<Arg*>& args() const { return _args; }
    void set_args(const Span<Arg*>& args) { _args = args; }

    Arg* arg(size_t index) const { return _args.at(index); }

    size_t name() const { return _name; }
    void set_name(size_t name) { _name = name; }

    void insert_before(Inst* before, Inst* inst) {
      _insts.insert_before(before, inst);
    }

    void add(Inst* inst) {
      _insts.add(inst);
    }

    Inst* terminator() const;
    std::vector<Block*> successors() const;

    void autoname(size_t& next_name) {
      for (Arg* arg : _args) {
        arg->set_name(next_name++);
      }

      for (Inst* inst : _insts) {
        inst->set_name(next_name++);
      }
    }

    void write_header(std::ostream& stream) {
      stream << "b" << _name;
      if (_args.size() > 0) {
        stream << "(";
        bool is_first = true;
        for (Arg* arg : _args) {
          if (is_first) {
            is_first = false;
          } else {
            stream << ", ";
          }
          arg->write_arg(stream);
          stream << ": " << arg->type();
        }
        stream << ")";
      }
      stream << ":";
    }

    void write(std::ostream& stream, InfoWriter* info_writer = nullptr) {
      write_header(stream);
      stream << '\n';
      for (Inst* inst : _insts) {
        stream << "  ";
        inst->write_stmt(stream);
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
      Pure = 1 << 0,
      InBounds = 1 << 1
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
      write_flag(InBounds)

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
    LinkedList<Block> _blocks;
    size_t _block_count = 0;
    size_t _name_count = 0;
  public:
    Section(Context& context, Allocator& allocator):
      _context(context), _allocator(allocator) {}

    Context& context() const { return _context; }
    Allocator& allocator() const { return _allocator; }

    auto begin() { return _blocks.begin(); }
    auto end() { return _blocks.end(); }

    Block* entry() const { return _blocks.first(); }

    auto range() { return _blocks.range(); }
    auto rev_range() { return _blocks.rev_range(); }

    size_t block_count() const { return _block_count; }
    size_t name_count() const { return _name_count; }

    void add(Block* block) { _blocks.add(block); }

    void insert_before(Block* before, Block* block) {
      _blocks.insert_before(before, block);
    }

    void autoname() {
      _name_count = 0;
      _block_count = 0;
      for (Block* block : _blocks) {
        block->set_name(_block_count++);
        block->autoname(_name_count);
      }
    }

    void write(std::ostream& stream, InfoWriter* info_writer = nullptr) {
      autoname();
      
      stream << "section {\n";
      for (Block* block : _blocks) {
        block->write(stream, info_writer);
      }
      stream << "}\n";
    }

    bool verify(std::ostream& errors) {
      autoname();

      std::set<Value*> defined;
      for (Block* block : *this) {
        for (Arg* arg : block->args()) {
          defined.insert(arg);
        }

        for (Inst* inst : *block) {
          for (Value* arg : inst->args()) {
            if (arg == nullptr) {
              errors << "Instruction ";
              inst->write_arg(errors);
              errors << " has null argument\n";
              return true;
            }

            if (arg->is_inst() &&
                defined.find(arg) == defined.end()) {
              errors << "Instruction ";
              inst->write_arg(errors);
              errors << " uses undefined value ";
              arg->write_arg(errors);
              errors << "\n";
              return true;
            }
          }

          defined.insert(inst);
        }

        if (!block->terminator()) {
          errors << "Block ";
          block->write_arg(errors);
          errors << " has no terminator\n";
          return true;
        }

        if (dynmatch(JumpInst, jump, block->terminator())) {
          if (jump->args().size() != jump->block()->args().size()) {
            errors << "Block ";
            block->write_arg(errors);
            errors << " jumps to block ";
            jump->block()->write_arg(errors);
            errors << " which requires ";
            errors << jump->block()->args().size();
            errors << " arguments, but ";
            errors << jump->args().size();
            errors << " were provided\n";
            return true;
          }

          for (Arg* arg : jump->block()->args()) {
            if (arg->type() != jump->arg(arg->index())->type()) {
              errors << "Block ";
              block->write_arg(errors);
              errors << " jumps to block ";
              jump->block()->write_arg(errors);
              errors << " with formal argument ";
              arg->write_arg(errors);
              errors << " of type ";
              errors << arg->type();
              errors << ", but provided argument ";
              jump->arg(arg->index())->write_arg(errors);
              errors << " has type ";
              errors << jump->arg(arg->index())->type();
              errors << "\n";
              return true;
            }
          }
        } else {
          for (Block* succ : block->successors()) {
            if (succ->args().size() != 0) {
              errors << "Block ";
              block->write_arg(errors);
              errors << " jumps to block ";
              succ->write_arg(errors);
              errors << " which requires ";
              errors << succ->args().size();
              errors << " arguments, but none were provided\n";
              return true;
            }
          }
        }
      }
      return false;
    }
  };

  class Builder {
  private:
    Section* _section = nullptr;
    Block* _block = nullptr;
    Inst* _before = nullptr;
  public:
    Builder(Section* section): _section(section) {}

    Section* section() const { return _section; }
    Block* block() const { return _block; }
    Inst* before() const { return _before; }

    void move_to_end(Block* block) {
      _block = block;
      _before = nullptr;
    }

    void move_to_begin(Block* block) {
      _block = block;
      _before = block->empty() ? nullptr : *block->begin();
    }

    void move_prev() {
      if (_before) {
        _before = _before->prev();
      } else {
        _before = *_block->rbegin();
      }
    }

    void move_next() {
      if (_before) {
        _before = _before->next();
      }
    }

    void move_before(Block* block, Inst* inst) {
      _block = block;
      _before = inst;
    }

    Arg* entry_arg(size_t index) const {
      return _section->entry()->arg(index);
    }

    void insert(Inst* inst) {
      _block->insert_before(_before, inst);
    }

    Arg* alloc_arg(Type type, size_t index) {
      return new (_section->allocator().alloc<Arg>()) Arg(type, index);
    }

    Block* alloc_block() {
      return new (_section->allocator().alloc<Block>()) Block();
    }

    Block* alloc_block(size_t arg_count) {
      Block* block = (Block*) _section->allocator().alloc(
        sizeof(Block) + sizeof(Arg*) * arg_count, alignof(Block)
      );
      return new (block) Block(Span<Arg*>::trailing<Block>(block, arg_count).zeroed());
    }

    Block* alloc_block(const Span<Type>& arg_types) {
      Block* block = (Block*) _section->allocator().alloc(
        sizeof(Block) + sizeof(Arg*) * arg_types.size(), alignof(Block)
      );
      Span<Arg*> args = Span<Arg*>::trailing<Block>(block, arg_types.size());
      for (size_t it = 0; it < arg_types.size(); it++) {
        args[it] = alloc_arg(arg_types[it], it);
      }
      return new (block) Block(args);
    }

    Block* alloc_block(const Span<Arg*>& args) {
      Block* block = (Block*) _section->allocator().alloc(
        sizeof(Block) + sizeof(Arg*) * args.size(), alignof(Block)
      );
      Span<Arg*> trailing_args = Span<Arg*>::trailing<Block>(block, args.size());
      for (size_t it = 0; it < args.size(); it++) {
        trailing_args[it] = args[it];
      }
      return new (block) Block(trailing_args);
    }

    Block* alloc_block(const std::vector<Type>& arg_types) {
      return alloc_block(Span<Type>((Type*) arg_types.data(), arg_types.size())); 
    }

    Block* alloc_block(const std::vector<Arg*>& args) {
      return alloc_block(Span<Arg*>((Arg**) args.data(), args.size())); 
    }

    #define define_build_block(arg_type, arg_name) \
      Block* build_block(arg_type arg_name) { \
        Block* block = alloc_block(arg_name); \
        _section->add(block); \
        return block; \
      } \
      Block* build_block_before(Block* before, arg_type arg_name) { \
        Block* block = alloc_block(arg_name); \
        _section->insert_before(before, block); \
        return block; \
      }
    
    define_build_block(size_t, arg_count)
    define_build_block(const Span<Type>&, arg_types)
    define_build_block(const std::vector<Type>&, arg_types)
    define_build_block(const Span<Arg*>&, arg_types)
    define_build_block(const std::vector<Arg*>&, arg_types)

    #undef define_build_block

    Block* build_block() { return build_block(0); }
    Block* build_block_before(Block* before) { return build_block_before(before, 0); }

    Const* build_const(Type type, uint64_t value) {
      return _section->context().build_const(type, value);
    }

    Const* build_const_fast(Type type, uint64_t value) {
      Const* constant = (Const*) _section->allocator().alloc(sizeof(Const), alignof(Const));
      new (constant) Const(type, value);
      return constant;
    }

    ${builder}

    ShlInst* build_shl(Value* a, size_t shift) {
      assert(shift <= type_size(a->type()) * 8);
      return build_shl(a, build_const(a->type(), shift));
    }

    CommentInst* build_comment(const std::string& text) {
      return build_comment(_section->context().alloc_string(text));
    }

    JumpInst* build_jump(Block* block, const Span<Value*>& args) {
      JumpInst* jump = (JumpInst*) _section->allocator().alloc(
        sizeof(JumpInst) + sizeof(Value*) * args.size(),
        alignof(JumpInst)
      );
      new (jump) JumpInst(block);
      Span<Value*> trailing_span = Span<Value*>::trailing<JumpInst>(jump, args.size());
      for (size_t it = 0; it < args.size(); it++) {
        trailing_span[it] = args[it];
      }
      jump->set_args(trailing_span);
      insert(jump);
      return jump;
    }

    JumpInst* build_jump(Block* block, const std::vector<Value*>& args) {
      return build_jump(block, Span<Value*>((Value**) args.data(), args.size()));
    }

    // Folding

  private:
    template <class Fn>
    Value* fold_into_const_select(Value* value_a,
                                  Const* const_b,
                                  Type type,
                                  const Fn& const_prop) {
      
      // For a binary operator # and constants a, b and c:
      //   (cond ? a : b) # c => (cond ? (a # c) : (b # c))
      // where both (a # c) and (b # c) are constant folded.
      
      if (dynmatch(SelectInst, const_select, value_a)) {
        dynmatch(Const, true_const, const_select->arg(1));
        dynmatch(Const, false_const, const_select->arg(2));

        if (true_const && false_const) {
          uint64_t true_prop = const_prop(true_const, const_b);
          uint64_t false_prop = const_prop(false_const, const_b);

          if (true_prop == false_prop) {
            return build_const(type, true_prop);
          } else {
            return fold_select(
              const_select->cond(),
              build_const(type, true_prop),
              build_const(type, false_prop)
            );
          }
        }
      }
      return nullptr;
    }
  
  public:

    Value* fold_add(Value* a, Value* b) {
      if (dynamic_cast<Const*>(a)) {
        std::swap(a, b);
      }

      if (dynmatch(Const, const_b, b)) {
        if (const_b->value() == 0) {
          return a;
        }

        if (dynmatch(AddInst, add_a, a)) {
          if (dynmatch(Const, const_a_b, add_a->arg(1))) {
            return fold_add(
              add_a->arg(0),
              build_const(
                a->type(),
                (const_a_b->value() + const_b->value()) & type_mask(a->type())
              )
            );
          }
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

    Value* fold_div_s(Value* a, Value* b) {
      return build_div_s(a, b);
    }

    Value* fold_div_u(Value* a, Value* b) {
      return build_div_u(a, b);
    }

    Value* fold_mod_s(Value* a, Value* b) {
      return build_mod_s(a, b);
    }
  
  private:
    bool is_power_of_two(uint64_t value) {
      return value != 0 && (value & (value - 1)) == 0;
    }

  public:
    Value* fold_mod_u(Value* a, Value* b) {
      if (dynmatch(Const, const_b, b)) {
        if (is_power_of_two(const_b->value())) {
          uint64_t mask = const_b->value() - 1;
          return fold_and(a, build_const(a->type(), mask));
        }
      }

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
  
  private:
    XorInst* is_not(Value* value) {
      if (dynmatch(XorInst, xor_inst, value)) {
        if (dynmatch(Const, const_arg, xor_inst->arg(1))) {
          if (const_arg->value() == type_mask(value->type())) {
            return xor_inst;
          }
        }
      }
      return nullptr;
    }

  public:
    Value* fold_xor(Value* a, Value* b) {
      if (dynamic_cast<Const*>(a)) {
        std::swap(a, b);
      }

      if (dynmatch(Const, constant, b)) {
        if (constant->value() == 0) {
          return a;
        } else if (constant->value() == type_mask(a->type())) {
          // (a ^ -1) ^ -1 => a
          if (XorInst* not_a = is_not(a)) {
            return not_a->arg(0);
          }
        }
      }
      return build_xor(a, b);
    }

    Value* fold_not(Value* a) {
      return fold_xor(a, build_const(a->type(), type_mask(a->type())));
    }

    Value* fold_eq(Value* a, Value* b) {
      if (dynamic_cast<Const*>(a)) {
        std::swap(a, b);
      }

      if (a == b) {
        // (a == a) => 1
        return build_const(Type::Bool, 1);
      }

      if (dynmatch(Const, const_b, b)) {
        // (cond ? a : b) == c => (cond ? (a == c) : (b == c))
        Value* res = fold_into_const_select(a, const_b, Type::Bool, [](Const* a, Const* b){
          return a->value() == b->value() ? 1 : 0;
        });

        if (res) {
          return res;
        }

        if (a->type() == Type::Bool) {
          if (const_b->value()) {
            // a == 1 => a
            return a;
          } else {
            // a == 0 => !a
            return fold_not(a);
          }
        }
      }

      return build_eq(a, b);
    }

    Value* fold_ne(Value* a, Value* b) {
      return fold_not(fold_eq(a, b));
    }

    Value* fold_lt_s(Value* a, Value* b) {
      return build_lt_s(a, b);
    }

    Value* fold_lt_u(Value* a, Value* b) {
      if (dynmatch(Const, const_b, b)) {
        if (const_b->value() == 0) {
          // a < 0 => 0
          return build_const(Type::Bool, 0);
        }
      }

      return build_lt_u(a, b);
    }

    Value* fold_gt_s(Value* a, Value* b) {
      return fold_lt_s(b, a);
    }

    Value* fold_gt_u(Value* a, Value* b) {
      return fold_lt_u(b, a);
    }

    Value* fold_le_s(Value* a, Value* b) {
      return fold_not(fold_gt_s(a, b));
    }

    Value* fold_le_u(Value* a, Value* b) {
      return fold_not(fold_gt_u(a, b));
    }

    Value* fold_ge_s(Value* a, Value* b) {
      return fold_le_s(b, a);
    }

    Value* fold_ge_u(Value* a, Value* b) {
      return fold_le_u(b, a);
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

      if (XorInst* not_cond = is_not(cond)) {
        // (cond ^ -1 ? a : b) => (cond ? b : a)
        cond = not_cond->arg(0);
        std::swap(true_value, false_value);
      }

      if (true_value->type() == Type::Bool) {
        dynmatch(Const, true_const, true_value);
        dynmatch(Const, false_const, false_value);
        if (true_const && false_const) {
          if (true_const->value() == 1 && false_const->value() == 0) {
            // (cond ? 1 : 0) => cond
            return cond;
          } else if (true_const->value() == 0 && false_const->value() == 1) {
            // (cond ? 0 : 1) => !cond
            return fold_xor(cond, build_const(Type::Bool, 1));
          }
        }
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

    Value* fold_resize_s(Value* a, Type type) {
      if (a->type() == type) {
        return a;
      }
      return build_resize_s(a, type);
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

    Value* fold_jump(Block* block) {
      return build_jump(block);
    }

    Value* fold_branch(Value* cond, Block* true_block, Block* false_block) {
      if (XorInst* xor_inst = is_not(cond)) {
        // Branch !cond, a, b => Branch cond, b, a
        cond = xor_inst->arg(0);
        std::swap(true_block, false_block);
      }

      return build_branch(cond, true_block, false_block);
    }

    Value* fold_load(Value* ptr, Type type, LoadFlags flags, AliasingGroup aliasing, uint64_t offset) {
      if (dynmatch(AddPtrInst, add_ptr, ptr)) {
        if (dynmatch(Const, const_offset, add_ptr->offset())) {
          ptr = add_ptr->ptr();
          offset += const_offset->value();
        }
      }

      return build_load(ptr, type, flags, aliasing, offset);
    }

    Value* fold_store(Value* ptr, Value* value, AliasingGroup aliasing, uint64_t offset) {
      if (dynmatch(AddPtrInst, add_ptr, ptr)) {
        if (dynmatch(Const, const_offset, add_ptr->offset())) {
          ptr = add_ptr->ptr();
          offset += const_offset->value();
        }
      }

      return build_store(ptr, value, aliasing, offset);
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

    size_t real_size() const { return _data.size(); }

    T& operator[](size_t index) {
      if (index >= _data.size()) {
        _data.resize(index + 1);
      }
      return _data[index];
    }

    auto begin() { return _data.begin(); }
    auto end() { return _data.end(); }
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

    Value* build_mod_s(Value* a, Value* b) {
      return Builder::fold_mod_s(a, b);
    }

    Value* build_mod_u(Value* a, Value* b) {
      return Builder::fold_mod_u(a, b);
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

    Value* build_eq(Value* a, Value* b) {
      return Builder::fold_eq(a, b);
    }

    Value* build_lt_s(Value* a, Value* b) {
      return Builder::fold_lt_s(a, b);
    }

    Value* build_lt_u(Value* a, Value* b) {
      return Builder::fold_lt_u(a, b);
    }

    Value* build_resize_u(Value* a, Type type) {
      return Builder::fold_resize_u(a, type);
    }

    Value* build_shl(Value* a, Value* b) {
      return Builder::fold_shl(a, b);
    }

    Value* build_shr_u(Value* a, Value* b) {
      return Builder::fold_shr_u(a, b);
    }

    Value* build_shr_s(Value* a, Value* b) {
      return Builder::fold_shr_s(a, b);
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

    void init_store(Value* ptr, Value* value, AliasingGroup aliasing, uint64_t offset) {
      if (aliasing < 0) {
        _exact_memory[-aliasing] = value;
      } else {
        _memory[aliasing].store(ptr, offset, value);
      }
    }

  };

  ${capi}

  template<class T>
  class NameMap {
  private:
    T* _data = nullptr;
    size_t _size = 0;
  public:
    NameMap() {}
    NameMap(Section* section) { init(section); }

    NameMap(const NameMap<T>&) = delete;
    NameMap<T>& operator=(const NameMap<T>&) = delete;

    ~NameMap() { if (_data) { delete[] _data; } }

    void init(Section* section) {
      assert(_data == nullptr);
      _size = section->name_count();
      _data = new T[_size]();
    }

    T& at(NamedValue* value) {
      assert(value->name() < _size);
      return _data[value->name()];
    }

    T& operator[](NamedValue* value) { return at(value); }

    const T& at(NamedValue* value) const {
      assert(value->name() < _size);
      return _data[value->name()];
    }

    const T& operator[](NamedValue* value) const { return at(value); }

    T& at_name(size_t name) {
      assert(name < _size);
      return _data[name];
    }
  };

  void Inst::substitute_args(NameMap<Value*>& substs) {
    for (size_t it = 0; it < _args.size(); it++) {
      Value* arg = _args.at(it);
      if (arg->is_inst()) {
        if (substs[(Inst*) arg]) {
          set_arg(it, substs[(Inst*) arg]);
        }
      }
    }
  }

  template <class Self>
  class Pass {
  private:
  public:
    Pass(Section* section) {
      section->autoname();
    }

    ~Pass() {
    }

    template <class... Args>
    static void run(Args... args) {
      Self self(args...);
    }
  };

  class DeadCodeElim: public Pass<DeadCodeElim> {
  public:
    DeadCodeElim(Section* section): Pass(section) {
      NameMap<bool> used(section);

      for (Block* block : section->rev_range()) {
        for (Inst* inst : block->rev_range()) {
          if (used[inst] ||
              inst->has_side_effect() ||
              inst->is_terminator() ||
              dynamic_cast<CommentInst*>(inst)) {
            used[inst] = true;
            for (Value* arg : inst->args()) {
              if (arg->is_inst()) {
                used[(Inst*) arg] = true;
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

  inline bool could_alias(LoadInst* load, StoreInst* store) {
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

  class RefineAliasing: public Pass<RefineAliasing> {
  private:
    struct GroupInfo {
      bool is_invalid = false;
      Value* base = nullptr;
      Type type = Type::Void;
    };

    struct Key {
      AliasingGroup group;
      uint64_t offset;

      Key(AliasingGroup _group, uint64_t _offset):
        group(_group), offset(_offset) {}

      bool operator==(const Key& other) const {
        return group == other.group && offset == other.offset;
      }
    };

    struct KeyHash {
      size_t operator()(const Key& key) const {
        return std::hash<AliasingGroup>()(key.group) ^ std::hash<uint64_t>()(key.offset);
      }
    };

    ExpandingVector<GroupInfo> _groups;
    AliasingGroup _min_exact_group = 0;
    std::vector<LoadInst*> _loads;
    std::vector<StoreInst*> _stores;
    std::unordered_map<Key, AliasingGroup, KeyHash> _exact_groups;

    void access(AliasingGroup aliasing, Value* ptr, uint64_t offset, Type type) {
      assert(aliasing >= 0);
      GroupInfo& group = _groups[aliasing];
      if (!group.is_invalid) {
        if (group.base == nullptr) {
          group.base = ptr;
          group.type = type;
        } else if (group.base != ptr || group.type != type || offset % type_size(type) != 0) {
          group.is_invalid = true;
        }
      }
    }

    AliasingGroup apply(AliasingGroup aliasing, uint64_t offset) {
      assert(aliasing >= 0);
      GroupInfo& group = _groups[aliasing];
      if (group.is_invalid) {
        return aliasing;
      }

      Key key(aliasing, offset);
      if (_exact_groups.find(key) == _exact_groups.end()) {
        _exact_groups[key] = --_min_exact_group;
      }
      return _exact_groups[key];
    }
  public:
    RefineAliasing(Section* section): Pass(section) {
      for (Block* block : *section) {
        for (Inst* inst : *block) {
          if (dynmatch(LoadInst, load, inst)) {
            if (load->aliasing() >= 0) {
              _loads.push_back(load);
              access(load->aliasing(), load->ptr(), load->offset(), inst->type());
            } else {
              _min_exact_group = std::min(_min_exact_group, load->aliasing());
            }
          } else if (dynmatch(StoreInst, store, inst)) {
            if (store->aliasing() >= 0) {
              _stores.push_back(store);
              access(store->aliasing(), store->ptr(), store->offset(), store->value()->type());
            } else {
              _min_exact_group = std::min(_min_exact_group, store->aliasing());
            }
          }
        }
      }

      for (LoadInst* load : _loads) {
        load->set_aliasing(apply(load->aliasing(), load->offset()));
      }

      for (StoreInst* store : _stores) {
        store->set_aliasing(apply(store->aliasing(), store->offset()));
      }
    }
  };

  class DeadStoreElim: public Pass<DeadStoreElim> {
  public:
    DeadStoreElim(Section* section): Pass(section) {
      NameMap<bool> unused(section);
      ExpandingVector<StoreInst*> last_store;

      for (Block* block : *section) {
        for (Inst* inst : *block) {
          if (dynmatch(StoreInst, store, inst)) {
            Pointer pointer(store->ptr(), store->offset());
            if (store->aliasing() < 0) {
              last_store[-store->aliasing()] = store;
              unused[store] = true;
            }
          } else if (dynmatch(LoadInst, load, inst)) {
            if (load->aliasing() < 0) {
              StoreInst* store = last_store[-load->aliasing()];
              if (store) {
                unused[store] = false;
              }
            }
          }
        }

        for (StoreInst*& store : last_store) {
          if (store) {
            unused[store] = false;
            store = nullptr; // Clear for next block
          }
        }

        block->filter_inplace([&](Inst* inst) {
          return !unused[inst];
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

      static Bits constant(Type type, uint64_t value) {
        return Bits(type, type_mask(type), value);
      }

      static Bits constant(bool value) {
        return Bits::constant(Type::Bool, value ? 1 : 0);
      }

      static Bits constant(void* ptr) {
        return Bits::constant(Type::Ptr, (uint64_t)(uintptr_t) ptr);
      }

      bool is_known(size_t bit) const {
        return (mask & (uint64_t(1) << bit)) != 0;
      }

      std::optional<bool> at(size_t bit) const {
        if (mask & (uint64_t(1) << bit)) {
          return (value & (uint64_t(1) << bit)) != 0;
        }
        return {};
      }

      bool is_const() const {
        return mask == type_mask(type);
      }

      static Bits div_u(Type type, uint64_t a, uint64_t b) {
        return Bits::constant(type, a / b);
      }

      static Bits div_s(Type type, uint64_t a, uint64_t b) {
        return Bits(type, 0, 0);  // TODO
      }

      static Bits mod_u(Type type, uint64_t a, uint64_t b) {
        return Bits::constant(type, a % b);
      }

      static Bits mod_s(Type type, uint64_t a, uint64_t b) {
        return Bits(type, 0, 0);  // TODO
      }

      static Bits lt_u(Type type, uint64_t a, uint64_t b) {
        return Bits::constant(a < b);
      }

      static Bits lt_s(Type type, uint64_t a, uint64_t b) {
        bool res = false;
        switch (type) {
          case Type::Int8: res = (int8_t(a) < int8_t(b)); break;
          case Type::Int16: res = (int16_t(a) < int16_t(b)); break;
          case Type::Int32: res = (int32_t(a) < int32_t(b)); break;
          case Type::Int64: res = (int64_t(a) < int64_t(b)); break;
          default: assert(false); // Unsupported type
        }
        return Bits::constant(res);
      }

      #define const_binop(name, expr) \
        Bits name(const Bits& other) const { \
          if (is_const() && other.is_const()) { \
            return expr; \
          } \
          return Bits(type, 0, 0); \
        }
      
      const_binop(operator+, Bits::constant(type, value + other.value))
      const_binop(operator-, Bits::constant(type, value - other.value))
      const_binop(operator*, Bits::constant(type, value * other.value))

      const_binop(div_u, div_u(type, value, other.value))
      const_binop(div_s, div_s(type, value, other.value))
      const_binop(mod_u, mod_u(type, value, other.value))
      const_binop(mod_s, mod_s(type, value, other.value))

      const_binop(eq, Bits::constant(Type::Bool, value == other.value))
      const_binop(lt_u, lt_u(type, value, other.value))
      const_binop(lt_s, lt_s(type, value, other.value))

      #undef const_binop

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

      Bits operator^(const Bits& other) const {
        return Bits(
          type,
          mask & other.mask,
          value ^ other.value
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

      Bits shr_s(size_t shift) const {
        Bits result(
          type,
          (mask >> shift),
          value >> shift
        );
        std::optional<bool> sign_bit = at(type_width(type) - 1);
        if (sign_bit.has_value()) {
          uint64_t upper_bits = type_mask(type) & ~(type_mask(type) >> shift);
          result.mask |= upper_bits;
          if (sign_bit.value()) {
            result.value |= upper_bits;
          } else {
            result.value &= ~upper_bits;
          }
        }
        return result;
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
      shift_by_const(shr_s)

      #undef shift_by_const

      Bits resize_u(Type to) const {
        return Bits(
          to,
          (mask & type_mask(type) & type_mask(to)) | (type_mask(to) & ~type_mask(type)),
          value & type_mask(type) & type_mask(to)
        );
      }

      Bits resize_s(Type to) const {
        Bits result(
          to,
          mask & type_mask(type) & type_mask(to),
          value & type_mask(type) & type_mask(to)
        );
        std::optional<bool> sign_bit = at(type_width(type) - 1);
        if (sign_bit.has_value()) {
          uint64_t upper_bits = type_mask(to) & ~type_mask(type);
          result.mask |= upper_bits;
          if (sign_bit.value()) {
            result.value |= upper_bits;
          } else {
            result.value &= ~upper_bits;
          }
        }
        return result;
      }

      Bits resize_x(Type to) const {
        return Bits(
          to,
          mask & type_mask(type) & type_mask(to),
          value
        );
      }

      Bits select(const Bits& a, const Bits& b) const {
        if (is_const()) {
          if (value != 0) {
            return a;
          } else {
            return b;
          }
        }

        return Bits(
          a.type,
          a.mask & b.mask,
          a.value
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

      static Bits at(const NameMap<Bits>& values, Value* value) {
        if (dynmatch(Const, constant, value)) {
          return Bits(
            constant->type(),
            type_mask(constant->type()),
            constant->value()
          );
        } else if (value->is_named()) {
          return values.at((NamedValue*) value);
        } else {
          assert(false); // Unreachable
          return Bits();
        }
      }

      static Bits eval(Inst* inst, NameMap<Bits>& values) {
        if (dynamic_cast<FreezeInst*>(inst) ||
            dynamic_cast<AssumeConstInst*>(inst)) {
          return at(values, inst->arg(0));
        } else if (dynmatch(SelectInst, select, inst)) {
          Bits cond = at(values, select->cond());
          Bits a = at(values, select->arg(1));
          Bits b = at(values, select->arg(2));
          return cond.select(a, b);
        } else if (dynmatch(ResizeUInst, resize_u, inst)) {
          Bits a = at(values, resize_u->arg(0));
          return a.resize_u(resize_u->type());
        } else if (dynmatch(ResizeSInst, resize_u, inst)) {
          Bits a = at(values, resize_u->arg(0));
          return a.resize_s(resize_u->type());
        } else if (dynmatch(ResizeXInst, resize_x, inst)) {
          Bits a = at(values, resize_x->arg(0));
          return a.resize_x(resize_x->type());
        }

        #define binop(name, expr) \
          else if (dynmatch(name, binop, inst)) { \
            Bits a = at(values, binop->arg(0)); \
            Bits b = at(values, binop->arg(1)); \
            return expr; \
          }
        
        binop(AddPtrInst, a + b)
        binop(AddInst, a + b)
        binop(SubInst, a - b)
        binop(MulInst, a * b)
        binop(DivSInst, a.div_s(b))
        binop(DivUInst, a.div_u(b))
        binop(ModSInst, a.mod_s(b))
        binop(ModUInst, a.mod_u(b))

        binop(AndInst, a & b)
        binop(OrInst, a | b)
        binop(XorInst, a ^ b)
        
        binop(ShlInst, a.shl(b))
        binop(ShrUInst, a.shr_u(b))
        binop(ShrSInst, a.shr_s(b))

        binop(EqInst, a.eq(b))
        binop(LtSInst, a.lt_s(b))
        binop(LtUInst, a.lt_u(b))

        #undef binop

        else {
          return Bits(inst->type(), 0, 0);
        }
      }
    };

  private:
    Section* _section;
    NameMap<Bits> _values;
  public:
    KnownBits(Section* section): _section(section), _values(section) {
      for (Block* block : *section) {
        for (Arg* arg : block->args()) {
          _values[arg] = Bits(arg->type(), 0, 0);
        }

        for (Inst* inst : *block) {
          _values[inst] = Bits::eval(inst, _values);
        }
      }
    }

    Bits at(Value* value) const {
      return Bits::at(_values, value);
    }

    void write(std::ostream& stream) {
      InfoWriter info_writer([&](std::ostream& stream, Inst* inst){
        _values[inst].write(stream);
      });
      _section->write(stream, &info_writer);
    }
  };

  class Interpreter {
  public:
    using Bits = KnownBits::Bits;
  private:
    Section* _section;
    NameMap<Bits> _values;

    bool _refine_store_values = true;

    // Program Counter
    Block* _block = nullptr;
    Inst* _inst = nullptr;
  public:
    Interpreter(Section* section, const std::vector<Bits>& entry_args):
        _section(section) {
      
      _section->autoname();
      _values.init(_section);
      enter(section->entry(), entry_args);
    }

    Section* section() const { return _section; }
    Block* block() const { return _block; }
    Inst* inst() const { return _inst; }

    bool is_valid() const {
      return _block != nullptr && _inst != nullptr;
    }

    enum class Event {
      None,
      // Info
      Exit, EnterBlock, RefineStoreValue,
      // Errors
      BranchUnknown, MemoryUnknown,
      Invalid
    };

    static bool is_error(Event event) {
      return (size_t) event >= (size_t) Event::BranchUnknown;
    }

    static const char* event_name(Event event) {
      static const char* names[] = {
        "None",
        "Exit",
        "EnterBlock",
        "RefineStoreValue",
        "BranchUnknown",
        "MemoryUnknown",
        "Invalid"
      };
      return names[(size_t) event];
    }

    void invalidate() {
      _block = nullptr;
      _inst = nullptr;
    }

    Event run_until(Event event) {
      while (true) {
        Event step_event = step();
        if (step_event == event ||
            step_event == Event::Exit ||
            is_error(step_event)) {
          return step_event;
        }
      }
    }

    Event run_for(size_t steps) {
      for (size_t it = 0; it < steps; it++) {
        Event step_event = step();
        if (step_event == Event::Exit ||
            is_error(step_event)) {
          return step_event;
        }
      }
      return Event::None;
    }

    Event run() {
      return run_until(Event::Exit);
    }

    Event step() {
      if (!is_valid()) {
        return Event::Invalid;
      }

      if (dynmatch(LoadInst, load, _inst)) {
        Bits ptr_bits = at(load->ptr());
        if (!ptr_bits.is_const()) {
          invalidate();
          return Event::MemoryUnknown;
        }
        uint8_t* ptr = (uint8_t*) ptr_bits.value + load->offset();
        uint64_t value = 0;
        switch (type_size(load->type())) {
          case 1: value = *ptr; break;
          case 2: value = *(uint16_t*) ptr; break;
          case 4: value = *(uint32_t*) ptr; break;
          case 8: value = *(uint64_t*) ptr; break;
          default:
            assert(false); // Unreachable
        }
        _values[load] = Bits::constant(load->type(), value);
        _inst = _inst->next();
      } else if (dynmatch(StoreInst, store, _inst)) {
        Bits ptr_bits = at(store->ptr());
        Bits value_bits = at(store->value());
        bool refined = false;
        if (_refine_store_values && !value_bits.is_const()) {
          refined = true;
          value_bits.mask = type_mask(store->value()->type());
        }
        if (!ptr_bits.is_const() || !value_bits.is_const()) {
          invalidate();
          return Event::MemoryUnknown;
        }
        uint8_t* ptr = (uint8_t*) ptr_bits.value + store->offset();
        uint64_t value = value_bits.value;
        switch (type_size(store->value()->type())) {
          case 1: *ptr = (uint8_t) value; break;
          case 2: *(uint16_t*) ptr = (uint16_t) value; break;
          case 4: *(uint32_t*) ptr = (uint32_t) value; break;
          case 8: *(uint64_t*) ptr = (uint64_t) value; break;
          default:
            assert(false); // Unreachable
        }
        _values[store] = Bits();
        _inst = _inst->next();
        if (refined) {
          return Event::RefineStoreValue;
        }
      } else if (dynmatch(JumpInst, jump, _inst)) {
        std::vector<Bits> args;
        for (Value* arg : jump->args()) {
          args.push_back(at(arg));
        }
        enter(jump->block(), args);
        return Event::EnterBlock;
      } else if (dynmatch(BranchInst, branch, _inst)) {
        Bits cond = at(branch->cond());
        if (!cond.is_const()) {
          invalidate();
          return Event::BranchUnknown;
        }
        if (cond.value != 0) {
          enter(branch->true_block(), {});
        } else {
          enter(branch->false_block(), {});
        }
        return Event::EnterBlock;
      } else if (dynmatch(ExitInst, exit, _inst)) {
        invalidate();
        return Event::Exit;
      } else {
        _values[_inst] = KnownBits::Bits::eval(_inst, _values);
        _inst = _inst->next();
      }
      return Event::None;
    }

    void enter(Block* block, const std::vector<Bits>& args) {
      assert(args.size() == block->args().size());
      _block = block;
      _inst = *block->begin();
      for (Arg* arg : block->args()) {
        _values[arg] = args[arg->index()];
      }
    }

    Bits at(Value* value) const {
      return KnownBits::Bits::at(_values, value);
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
    NameMap<Bits> _values;

    void use(Value* value, uint64_t used) {
      if (value->is_inst()) {
        Inst* inst = (Inst*) value;
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

    void use_all(Value* value) {
      use(value, Bits::all(value->type()));
    }

    void use_all_args(Inst* inst) {
      for (Value* arg : inst->args()) {
        use_all(arg);
      }
    }
  public:
    UsedBits(Section* section): _section(section), _values(section) {
      for (Block* block : section->rev_range()) {
        for (Inst* inst : block->rev_range()) {
          if (_values[inst].type != inst->type()) {
            assert(_values[inst].type == Type::Void);
            _values[inst] = Bits(inst->type(), 0);
          }
          
          if (dynmatch(ResizeUInst, resize_u, inst)) {
            use(resize_u->arg(0), _values[inst].used);
          } else if (dynmatch(ResizeXInst, resize_x, inst)) {
            use(resize_x->arg(0), _values[inst].used);
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
          } else if (dynamic_cast<AddInst*>(inst) ||
                     dynamic_cast<SubInst*>(inst) ||
                     dynamic_cast<MulInst*>(inst)) {
            uint64_t used = _values[inst].used;
            for (size_t it = 1; it < 64; it *= 2) {
              used |= used >> it;
            }
            for (Value* arg : inst->args()) {
              use(arg, used);
            }
          } else if (dynamic_cast<ShrUInst*>(inst) ||
                     dynamic_cast<ShrSInst*>(inst)) {
            if (dynmatch(Const, const_b, inst->arg(1))) {
              if (const_b->value() < type_size(inst->type()) * 8) {
                use(inst->arg(0), (_values[inst].used << const_b->value()) & type_mask(inst->type()));  
              } else {
                use(inst->arg(0), 0);
              }
              use_all(inst->arg(1));
            } else {
              use_all_args(inst);
            }
          } else {
            if (inst->has_side_effect() || inst->is_terminator() || _values[inst].used != 0) {
              use_all_args(inst);
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
      if (value->is_inst()) {
        return _values.at((Inst*) value);
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
    NameMap<std::vector<Use>> _uses;
  public:
    Uses(Section* section): _section(section), _uses(section) {
      for (Block* block : *section) {
        for (Inst* inst : *block) {
          for (size_t it = 0; it < inst->arg_count(); it++) {
            Value* arg = inst->arg(it);
            if (arg->is_inst()) {
              _uses[(Inst*) arg].emplace_back(inst, it);
            }
          }
        }
      }
    }

    const std::vector<Use>& at(Inst* inst) const {
      return _uses.at(inst);
    }
  };

  class Simplify: public Pass<Simplify> {
  private:
    Section* _section = nullptr;
    Builder _builder;

    template <class Fn>
    bool substitute(const Fn& fn) {
      NameMap<Value*> substs(_section);
      bool changed = false;
      for (Block* block : *_section) {
        for (auto inst_it = block->begin(); inst_it != block->end(); ) {
          Inst* inst = *inst_it;
          inst->substitute_args(substs);

          _builder.move_before(block, inst);
          Value* subst = fn(inst);
          if (subst) {
            substs[inst] = subst;
            inst_it = inst_it.erase();
            changed = true;
          } else {
            inst_it++;
          }
        }
      }
      return changed;
    }
  public:
    Simplify(Section* section, size_t max_iters):
        Pass(section), _section(section), _builder(section) {
      
      bool changed = true;
      for (size_t iter = 0; changed && iter < max_iters; iter++) {
        changed = false;

        section->autoname();
        KnownBits known_bits(section);

        changed |= substitute([&](Inst* inst) -> Value* {
          if (dynmatch(AndInst, and_inst, inst)) {
            KnownBits::Bits a = known_bits.at(and_inst->arg(0));
            KnownBits::Bits b = known_bits.at(and_inst->arg(1));

            if (b.is_const() && ((b.value ^ type_mask(b.type)) & (~a.mask | a.value)) == 0) {
              return and_inst->arg(0);
            }
          }
          return nullptr;
        });

        section->autoname();
        UsedBits used_bits(section);
        
        changed |= substitute([&](Inst* inst) -> Value* {
          if (dynmatch(AndInst, and_inst, inst)) {
            UsedBits::Bits used = used_bits.at(inst);

            if (dynmatch(Const, b, and_inst->arg(1))) {
              if ((used.used & ~b->value()) == 0) {
                return and_inst->arg(0);
              }
            }
          } else if (dynmatch(OrInst, or_inst, inst)) {
            UsedBits::Bits used = used_bits.at(inst);

            if (dynmatch(Const, b, or_inst->arg(1))) {
              if ((used.used & b->value()) == 0) {
                return or_inst->arg(0);
              }
            }
          } else if (dynamic_cast<ResizeUInst*>(inst) ||
                     dynamic_cast<ResizeSInst*>(inst)) {
            UsedBits::Bits used = used_bits.at(inst);
            uint64_t mask = type_mask(inst->type()) & type_mask(inst->arg(0)->type());
            if ((used.used & ~mask) == 0) {
              return _builder.build_resize_x(inst->arg(0), inst->type());
            }
          }
          return nullptr;
        });
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
        std::unordered_map<AliasingGroup, std::vector<LoadInst*>> valid_loads;

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

          if (dynmatch(StoreInst, store, inst)) {
            std::vector<LoadInst*> remaining_loads;
            for (LoadInst* load : valid_loads[store->aliasing()]) {
              if (could_alias(load, store)) {
                assert(canon.find(Lookup(load)) != canon.end());
                canon.erase(Lookup(load));
              } else {
                remaining_loads.push_back(load);
              }
            }
            valid_loads[store->aliasing()] = remaining_loads;
          }

          if (inst->has_side_effect() ||
              inst->is_terminator() ||
              dynamic_cast<CommentInst*>(inst)) {
            inst_it++;
            continue;
          }
          
          Lookup lookup(inst);
          if (canon.find(lookup) == canon.end()) {
            canon[lookup] = inst;
            if (dynmatch(LoadInst, load, inst)) {
              valid_loads[load->aliasing()].push_back(load);
            }
            inst_it++;
          } else {
            substs[inst] = canon.at(lookup);
            inst_it = inst_it.erase();
          }
        }
      }
    }
  };

  class Loop {
  private:
    Section* _section = nullptr;
    Block* _header = nullptr;
    Block* _extent = nullptr;
    Block* _preheader = nullptr;
  public:
    Loop(Section* section, Block* header, Block* extent):
        _section(section), _header(header), _extent(extent) {}

    Section* section() const { return _section; }

    Block* header() const { return _header; }
    Block* extent() const { return _extent; }

    Block* preheader() const { return _preheader; }
    void set_preheader(Block* preheader) { _preheader = preheader; }

    using iterator = decltype(_section->begin());

    Range<iterator> range() {
      iterator begin = _section->begin().at(_header);
      iterator end = _section->begin().at(_extent);
      end++;
      return Range(begin, end);
    }

    size_t first_name() const {
      return (*_header->begin())->name();
    }
  };

  class LoopInvCodeMotion: public Pass<LoopInvCodeMotion> {
  private:
    Loop* _loop;
    NameMap<bool> _invariant;
  public:
    LoopInvCodeMotion(Loop* loop):
        Pass(loop->section()),
        _loop(loop),
        _invariant(loop->section()) {
      
      assert(loop->preheader());
      assert(loop->preheader()->terminator());

      // ExpandingVector<bool> does not work due to std::vector<bool>
      ExpandingVector<uint8_t> stores;
      for (Block* block : loop->range()) {
        for (Inst* inst : *block) {
          if (dynmatch(StoreInst, store, inst)) {
            if (store->aliasing() < 0) {
              stores[-store->aliasing()] = 1;
            }
          }
        }
      }

      Builder builder(loop->section());
      builder.move_before(loop->preheader(), loop->preheader()->terminator());

      for (Block* block : loop->range()) {
        for (auto inst_it = block->begin(); inst_it != block->end(); ) {
          Inst* inst = *inst_it;

          if (inst->has_side_effect() ||
              inst->is_terminator() ||
              dynamic_cast<StoreInst*>(inst) ||
              dynamic_cast<CommentInst*>(inst)) {
            inst_it++;
            continue;
          }

          bool invariant = true;

          for (Value* arg : inst->args()) {
            if (!is_invariant(arg)) {
              invariant = false;
              break;
            }
          }

          if (invariant) {
            if (dynmatch(LoadInst, load, inst)) {
              if (load->flags().has(LoadFlags::InBounds) &&
                  load->aliasing() < 0) {
                if (stores[-load->aliasing()]) {
                  invariant = false;
                }
              } else {
                invariant = false;
              }
            }
          }

          if (invariant) {
            _invariant[inst] = true;
            inst_it = inst_it.erase();
            builder.insert(inst);
          } else {
            inst_it++;
          }
        }
      }
    }

    bool is_invariant(Value* value) const {
      if (value->is_inst()) {
        Inst* inst = (Inst*) value;
        if (inst->name() < _loop->first_name()) {
          return true;
        } else {
          return _invariant.at(inst);
        }
      }
      return true;
    }
  };

  class ConstnessAnalysis {
  public:
    static constexpr size_t ALWAYS = 0;
  private:
    Section* _section;
    NameMap<size_t> _groups;
    size_t _next_group = 1;
  public:
    ConstnessAnalysis(Section* section):
        _section(section),
        _groups(section) {
      
      for (Block* block : *section) {
        for (Arg* arg : block->args()) {
          _groups[arg] = _next_group++;
        }

        for (Inst* inst : *block) {
          if (dynmatch(FreezeInst, freeze, inst)) {
            _groups[inst] = ALWAYS;
          } else if (dynmatch(AssumeConstInst, assume_const, inst)) {
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
                dynamic_cast<SelectInst*>(inst)) {
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
      } else if (value->is_named()) {
        return _groups.at((NamedValue*) value);
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
    NameMap<bool> _can_trace_inst;
    NameMap<bool> _can_trace_const;

    void used_by(NamedValue* value, NamedValue* by) {
      if (_can_trace_inst.at(by)) {
        if (_constness.at(by) != _constness.at(value) ||
            (is_int_or_bool(value->type()) && !is_int_or_bool(by->type()))) {
          _can_trace_const[value] = true;
        }
        if (_constness.at(value) != ConstnessAnalysis::ALWAYS ||
            !is_int_or_bool(value->type())) {
          _can_trace_inst[value] = true;
        }
      }

      // Args cannot generate new constants, so all arguments need to be const traceable
      if (dynmatch(Arg, arg, by)) {
        if (_can_trace_const.at(by)) {
          _can_trace_const[value] = true;
        }
      }

      if (dynamic_cast<FreezeInst*>(by) ||
          (dynamic_cast<AssumeConstInst*>(by) && !is_int_or_bool(value->type()))) {
        _can_trace_inst[value] = true;
        _can_trace_const[value] = true;
      }
    }
  public:
    TraceCapabilities(Section* section, ConstnessAnalysis& constness):
        _section(section),
        _constness(constness),
        _can_trace_inst(section),
        _can_trace_const(section) {
    
      for (Block* block : section->rev_range()) {
        for (Inst* inst : block->rev_range()) {
          if (inst->has_side_effect() ||
              inst->is_terminator() ||
              dynamic_cast<FreezeInst*>(inst) ||
              dynamic_cast<AssumeConstInst*>(inst) ||
              dynamic_cast<CommentInst*>(inst)) {
            _can_trace_inst[inst] = true;
            _can_trace_const[inst] = true;
          }

          if (dynmatch(JumpInst, jump, inst)) {
            // Jump arguments are passed to block arguments
            for (Arg* block_arg : jump->block()->args()) {
              Value* arg = jump->arg(block_arg->index());
              if (arg->is_named()) {
                used_by((NamedValue*) arg, block_arg);
              }
            }
          } else {
            for (Value* arg : inst->args()) {
              if (arg->is_named()) {
                used_by((NamedValue*) arg, inst);
              }
            }
          }
        }
      }
    }

    bool can_trace_const(NamedValue* value) const {
      return _can_trace_const[value];
    }

    bool can_trace_inst(NamedValue* value) const {
      return _can_trace_inst[value];
    }

    bool any(NamedValue* value) const {
      return can_trace_const(value) || can_trace_inst(value);
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

