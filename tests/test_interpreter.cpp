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

#include "diff.hpp"

#include "../../unittest.cpp/unittest.hpp"

using namespace metajit;
using namespace metajit::test;

int main(int argc, char** argv) {
  InterpreterTestSuite suite("tests/output/test_interpreter", argc, argv);
  
  using Bits = Interpreter::Bits;
  using TestCase = InterpreterTest::TestCase;

  for (bool remainder : {false, true}) {
    suite.test(remainder ? "signed_remainder_overflow_produces_poison" :
                           "signed_division_overflow_produces_poison").run([remainder]() {
      for (Type type : {Type::Int64, Type::Int32, Type::Int16, Type::Int8}) {
        uint64_t min_value = uint64_t(1) << (type_width(type) - 1);
        Bits minimum = Bits::constant(type, min_value);
        Bits minus_one = Bits::constant(type, type_mask(type));
        auto evaluate = [&](Bits a, Bits b) {
          return remainder ? a.mod_s(b) : a.div_s(b);
        };
        Bits overflow = evaluate(minimum, minus_one);
        unittest_assert(overflow.is_poison);
        unittest_assert(overflow.type == type);
        Bits identity = evaluate(minimum, Bits::constant(type, 1));
        unittest_assert(!identity.is_poison);
        unittest_assert(identity.value == (remainder ? 0 : min_value));
        Bits adjacent = evaluate(Bits::constant(type, min_value + 1), minus_one);
        unittest_assert(!adjacent.is_poison);
        unittest_assert(adjacent.value == (remainder ? 0 : min_value - 1));
        Bits unsigned_result = remainder ? minimum.mod_u(minus_one) : minimum.div_u(minus_one);
        unittest_assert(!unsigned_result.is_poison);
        unittest_assert(unsigned_result.value == (remainder ? min_value : 0));
      }
    });
  }

  suite.test("zero_divisors_produce_poison").run([]() {
    for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
      Bits a = Bits::constant(type, 7);
      Bits zero = Bits::constant(type, 0);
      for (Bits result : {a.div_u(zero), a.div_s(zero), a.mod_u(zero), a.mod_s(zero)}) {
        unittest_assert(result.is_poison);
        unittest_assert(result.type == type);
      }
      Bits two = Bits::constant(type, 2);
      for (Bits result : {a.div_u(two), a.div_s(two)}) {
        unittest_assert(!result.is_poison && result.value == 3);
      }
      for (Bits result : {a.mod_u(two), a.mod_s(two)}) {
        unittest_assert(!result.is_poison && result.value == 1);
      }
    }
  });

  suite.test("oversized_shifts_produce_poison").run([]() {
    for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
      size_t width = type_width(type);
      Bits one = Bits::constant(type, 1);
      Bits ones = Bits::constant(type, type_mask(type));
      for (uint64_t shift : {uint64_t(width), uint64_t(width + 1), uint64_t(64), UINT64_MAX}) {
        Bits count = Bits::constant(type, shift);
        for (Bits result : {one.shl(count), one.shr_u(count), ones.shr_s(count)}) {
          unittest_assert(result.is_poison);
          unittest_assert(result.type == type);
        }
      }
      Bits zero = Bits::constant(type, 0);
      for (Bits result : {ones.shl(zero), ones.shr_u(zero), ones.shr_s(zero)}) {
        unittest_assert(!result.is_poison && result.value == type_mask(type));
      }
      Bits last = Bits::constant(type, width - 1);
      Bits left = one.shl(last);
      Bits logical = ones.shr_u(last);
      Bits arithmetic = ones.shr_s(last);
      unittest_assert(!left.is_poison && left.value == (uint64_t(1) << (width - 1)));
      unittest_assert(!logical.is_poison && logical.value == 1);
      unittest_assert(!arithmetic.is_poison && arithmetic.value == type_mask(type));
    }
  });

  suite.test("poison_integer_arithmetic").run([]() {
    for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
      for (Bits a : {Bits::constant(type, 7), Bits::poison(type)}) {
        for (Bits b : {Bits::poison(type), Bits::constant(type, 0), Bits::constant(type, 64)}) {
          if (!a.is_poison && !b.is_poison) continue;
          for (Bits result : {a + b, a - b, a * b, a.div_u(b), a.div_s(b),
                              a.mod_u(b), a.mod_s(b), a & b, a | b, a ^ b,
                              a.shl(b), a.shr_u(b), a.shr_s(b)}) {
            unittest_assert(result.is_poison);
            unittest_assert(result.type == type);
          }
          for (Bits result : {a.eq(b), a.lt_u(b), a.lt_s(b)}) {
            unittest_assert(result.is_poison);
            unittest_assert(result.type == Type::Bool);
          }
        }
      }
    }
  });

  suite.test("poison_float_arithmetic").run([]() {
    for (Type type : {Type::Float32, Type::Float64}) {
      Bits poison = Bits::poison(type);
      Bits zero = Bits::constant(type, 0);
      for (Bits a : {poison, zero}) {
        for (Bits b : {poison, zero}) {
          if (!a.is_poison && !b.is_poison) continue;
          for (Bits result : {a.add_f(b), a.sub_f(b), a.mul_f(b), a.div_f(b)}) {
            unittest_assert(result.is_poison);
            unittest_assert(result.type == type);
          }
          for (Bits result : {a.lt_f_u(b), a.lt_f_o(b)}) {
            unittest_assert(result.is_poison);
            unittest_assert(result.type == Type::Bool);
          }
        }
      }
    }
  });

  suite.interpreter_test("simple_add").run([](Builder& builder) -> std::vector<TestCase> {
    builder.move_to_end(builder.build_block({Type::Int32, Type::Int32}));
    Arg* a = builder.entry_arg(0);
    Arg* b = builder.entry_arg(1);
    Value* c = builder.build_add(a, b);
    builder.build_exit();

    return {
      TestCase(
        {Bits::constant(Type::Int32, 1), Bits::constant(Type::Int32, 2)},
        {{c, Bits::constant(Type::Int32, 3)}}
      ),
      TestCase(
        {Bits::constant(Type::Int32, 10), Bits::constant(Type::Int32, 20)},
        {{c, Bits::constant(Type::Int32, 30)}}
      ),
      TestCase(
        {Bits::constant(Type::Int32, -5), Bits::constant(Type::Int32, 15)},
        {{c, Bits::constant(Type::Int32, 10)}}
      ),
    };
  });

  suite.interpreter_test("poison_propagation").run([](Builder& builder) -> std::vector<TestCase> {
    builder.move_to_end(builder.build_block({Type::Int32, Type::Int32, Type::Int32}));
    Arg* a = builder.entry_arg(0);
    Arg* b = builder.entry_arg(1);
    Arg* c = builder.entry_arg(2);
    Value* x = builder.build_add(a, b);
    Value* y = builder.build_mul(x, c);
    builder.build_exit();

    return {
      TestCase(
        {Bits::constant(Type::Int32, 2), Bits::constant(Type::Int32, 3), Bits::constant(Type::Int32, 4)},
        {{x, Bits::constant(Type::Int32, 5)}, {y, Bits::constant(Type::Int32, 20)}}
      ),
      TestCase(
        {Bits::poison(Type::Int32), Bits::constant(Type::Int32, 2), Bits::constant(Type::Int32, 3)},
        {{x, Bits::poison(Type::Int32)}, {y, Bits::poison(Type::Int32)}}
      ),
      TestCase(
        {Bits::constant(Type::Int32, 1), Bits::poison(Type::Int32), Bits::constant(Type::Int32, 3)},
        {{x, Bits::poison(Type::Int32)}, {y, Bits::poison(Type::Int32)}}
      ),
      TestCase(
        {Bits::constant(Type::Int32, 1), Bits::constant(Type::Int32, 2), Bits::poison(Type::Int32)},
        {{x, Bits::constant(Type::Int32, 3)}, {y, Bits::poison(Type::Int32)}}
      ),
    };
  });

  suite.interpreter_test("poison_select").run([](Builder& builder) -> std::vector<TestCase> {
    builder.move_to_end(builder.build_block({Type::Bool, Type::Int32, Type::Int32}));
    Arg* cond = builder.entry_arg(0);
    Arg* a = builder.entry_arg(1);
    Arg* b = builder.entry_arg(2);
    Value* x = builder.build_select(cond, a, b);
    builder.build_exit();

    return {
      TestCase(
        {Bits::constant(true), Bits::constant(Type::Int32, 10), Bits::constant(Type::Int32, 20)},
        {{x, Bits::constant(Type::Int32, 10)}}
      ),
      TestCase(
        {Bits::constant(false), Bits::constant(Type::Int32, 10), Bits::constant(Type::Int32, 20)},
        {{x, Bits::constant(Type::Int32, 20)}}
      ),
      TestCase(
        {Bits::poison(Type::Bool), Bits::constant(Type::Int32, 10), Bits::constant(Type::Int32, 20)},
        {{x, Bits::poison(Type::Int32)}}
      ),
      TestCase(
        {Bits::constant(true), Bits::poison(Type::Int32), Bits::constant(Type::Int32, 20)},
        {{x, Bits::poison(Type::Int32)}}
      ),
      TestCase(
        {Bits::constant(false), Bits::poison(Type::Int32), Bits::constant(Type::Int32, 20)},
        {{x, Bits::constant(Type::Int32, 20)}}
      ),
      TestCase(
        {Bits::constant(true), Bits::constant(Type::Int32, 10), Bits::poison(Type::Int32)},
        {{x, Bits::constant(Type::Int32, 10)}}
      ),
      TestCase(
        {Bits::constant(false), Bits::constant(Type::Int32, 10), Bits::poison(Type::Int32)},
        {{x, Bits::poison(Type::Int32)}}
      ),
    };
  });

  return suite.finish();
}
