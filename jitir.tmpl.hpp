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

    void write(std::ostream& stream) {
      stream << "b" << _name << ":\n";
      for (Inst* inst : _insts) {
        stream << "  ";
        if (inst->type() != Type::Void) {
          inst->write_arg(stream);
          stream << " = ";
        }
        inst->write(stream);
        stream << '\n';
      }
    }

    void write_arg(std::ostream& stream) {
      stream << 'b' << _name;
    }
  };

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

    void write(std::ostream& stream) {
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
        block->write(stream);
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

    InputInst* build_input(Type type) {
      size_t id = _section->add_input(type);
      return build_input(id, type);
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
}

