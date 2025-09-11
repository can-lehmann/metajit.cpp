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

  ${insts}

  class Block {
  private:
    std::vector<Inst*> _insts;
  public:
    Block() {}

    auto begin() const { return _insts.begin(); }
    auto end() const { return _insts.end(); }

    void add(Inst* inst) {
      _insts.push_back(inst);
    }

    void autoname(size_t& next_name) {
      for (Inst* inst : _insts) {
        inst->set_name(next_name++);
      }
    }

    void write(std::ostream& stream) {
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
  };

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
      for (Block* block : _blocks) {
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
  };

  ${capi}


  template<class T>
  class ValueMap {
  private:
    std::vector<T> _data;
  public:
    ValueMap() {}
    ValueMap(Section* section) { init(section); }

    void init(Section* section) {
      _data.resize(section->name_count(), T());
    }

    T& at(Value* value) { return _data.at(value->name()); }
    T& operator[](Value* value) { return _data.at(value->name()); }

    const T& at(Value* value) const { return _data.at(value->name()); }
    const T& operator[](Value* value) const { return _data.at(value->name()); }
  };
}

