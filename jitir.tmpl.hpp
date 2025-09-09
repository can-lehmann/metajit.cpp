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

namespace jitir {
  enum class Type {
    Void, Bool, Int8, Int16, Int32, Int64, Ptr
  };

  inline bool is_int(Type type) {
    return type == Type::Int8 ||
           type == Type::Int16 ||
           type == Type::Int32 ||
           type == Type::Int64;
  }

  class Value {
  private:
    Type _type;
    size_t _name;
  public:
    Value(Type type): _type(type) {}

    Type type() const { return _type; }
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
      Value(value), _args(args) {}

    Value* arg(size_t index) const { return _args.at(arg); }

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

    void add(Inst* inst) {
      _insts.push_back(inst);
    }
  };

  class Function {
  private:
    std::vector<Block*> _block;
  public:
    Function() {}

  };

  class Builder {
  private:
    Block* _block;
  public:
    Builder(Block* block): _block(block) {}
    void insert(Inst* inst) { _block->add(inst); }

    ${builder}
  };

}
