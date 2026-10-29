// Copyright 2026 Can Joshua Lehmann
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
#include <cassert>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

enum class Type { Bool, Int8, Int16, Int32, Int64 };

inline uint64_t type_width(Type type) {
  switch (type) {
    case Type::Bool: return 1;
    case Type::Int8: return 8;
    case Type::Int16: return 16;
    case Type::Int32: return 32;
    case Type::Int64: return 64;
  }
  return 0;
}

inline uint64_t type_mask(Type type) {
  return type == Type::Int64 ? UINT64_MAX : (uint64_t(1) << type_width(type)) - 1;
}

inline bool is_int(Type type) { return type != Type::Bool; }
inline bool is_int_or_bool(Type) { return true; }

namespace metajit {
  inline uint64_t bit_reverse(Type type, uint64_t value) {
    uint64_t result = 0;
    for (uint64_t bit = 0; bit < type_width(type); ++bit) {
      result = (result << 1) | ((value >> bit) & 1);
    }
    return result;
  }
}

struct Value {
  Type _type;
  explicit Value(Type type): _type(type) {}
  virtual ~Value() {}
  Type type() const { return _type; }
};

struct Const: Value {
  uint64_t _value;
  Const(Type type, uint64_t value): Value(type), _value(value & type_mask(type)) {}
  uint64_t value() const { return _value; }
};

struct Inst: Value {
  std::vector<Value*> _args;
  Inst(Type type, std::initializer_list<Value*> args): Value(type), _args(args) {}
  Value* arg(size_t index) { return _args.at(index); }
};

#define RULEDSL_TEST_INST(name) struct name##Inst: Inst { using Inst::Inst; };
RULEDSL_TEST_INST(Add)
RULEDSL_TEST_INST(Sub)
RULEDSL_TEST_INST(Mul)
RULEDSL_TEST_INST(MulHighU)
RULEDSL_TEST_INST(DivS)
RULEDSL_TEST_INST(DivU)
RULEDSL_TEST_INST(ModS)
RULEDSL_TEST_INST(ModU)
RULEDSL_TEST_INST(And)
RULEDSL_TEST_INST(Or)
RULEDSL_TEST_INST(Xor)
RULEDSL_TEST_INST(Shl)
RULEDSL_TEST_INST(ShrS)
RULEDSL_TEST_INST(ShrU)
RULEDSL_TEST_INST(Eq)
RULEDSL_TEST_INST(LtS)
RULEDSL_TEST_INST(LtU)
RULEDSL_TEST_INST(Select)
RULEDSL_TEST_INST(ResizeU)
RULEDSL_TEST_INST(ResizeS)
RULEDSL_TEST_INST(ResizeX)
RULEDSL_TEST_INST(Popcount)
RULEDSL_TEST_INST(BitReverse)
RULEDSL_TEST_INST(Freeze)
#undef RULEDSL_TEST_INST

struct RawBuilder {
  std::vector<Value*> allocated;
  unsigned builds = 0;
  unsigned handwritten_calls = 0;

  ~RawBuilder() {
    for (Value* value: allocated) {
      delete value;
    }
  }

  template<class T> T* own(T* value) {
    allocated.push_back(value);
    ++builds;
    return value;
  }

  bool is_always_equal(Value* a, Value* b) {
    if (a == b) {
      return true;
    }
    Const* ca = dynamic_cast<Const*>(a);
    Const* cb = dynamic_cast<Const*>(b);
    return ca && cb && ca->type() == cb->type() && ca->value() == cb->value();
  }

  Const* build_const(Type type, uint64_t value) { return own(new Const(type, value)); }

  #define RULEDSL_TEST_BINARY(name, method) \
    name##Inst* build_##method(Value* a, Value* b) { \
      return own(new name##Inst(a->type(), {a, b})); \
    }
  RULEDSL_TEST_BINARY(Add, add)
  RULEDSL_TEST_BINARY(Sub, sub)
  RULEDSL_TEST_BINARY(Mul, mul)
  RULEDSL_TEST_BINARY(MulHighU, mul_high_u)
  RULEDSL_TEST_BINARY(DivS, div_s)
  RULEDSL_TEST_BINARY(DivU, div_u)
  RULEDSL_TEST_BINARY(ModS, mod_s)
  RULEDSL_TEST_BINARY(ModU, mod_u)
  RULEDSL_TEST_BINARY(And, and)
  RULEDSL_TEST_BINARY(Or, or)
  RULEDSL_TEST_BINARY(Xor, xor)
  RULEDSL_TEST_BINARY(Shl, shl)
  RULEDSL_TEST_BINARY(ShrS, shr_s)
  RULEDSL_TEST_BINARY(ShrU, shr_u)
  #undef RULEDSL_TEST_BINARY

  #define RULEDSL_TEST_COMPARE(name, method) \
    name##Inst* build_##method(Value* a, Value* b) { \
      return own(new name##Inst(Type::Bool, {a, b})); \
    }
  RULEDSL_TEST_COMPARE(Eq, eq)
  RULEDSL_TEST_COMPARE(LtS, lt_s)
  RULEDSL_TEST_COMPARE(LtU, lt_u)
  #undef RULEDSL_TEST_COMPARE

  #define RULEDSL_TEST_RESIZE(name, method) \
    name##Inst* build_##method(Value* a, Type type) { \
      return own(new name##Inst(type, {a})); \
    }
  RULEDSL_TEST_RESIZE(ResizeU, resize_u)
  RULEDSL_TEST_RESIZE(ResizeS, resize_s)
  RULEDSL_TEST_RESIZE(ResizeX, resize_x)
  #undef RULEDSL_TEST_RESIZE

  SelectInst* build_select(Value* condition, Value* a, Value* b) {
    return own(new SelectInst(a->type(), {condition, a, b}));
  }
  PopcountInst* build_popcount(Value* a) { return own(new PopcountInst(a->type(), {a})); }
  BitReverseInst* build_bit_reverse(Value* a) { return own(new BitReverseInst(a->type(), {a})); }
  FreezeInst* build_freeze(Value* a) { return own(new FreezeInst(a->type(), {a})); }
  Value* _fold_add(Value* a, Value* b) {
    ++handwritten_calls;
    return build_add(a, b);
  }
  Value* _fold_sub(Value* a, Value* b) {
    ++handwritten_calls;
    return build_sub(a, b);
  }
};
