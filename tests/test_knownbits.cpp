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

#include <bitset>
#include "diff.hpp"

#include "../../unittest.cpp/unittest.hpp"

using namespace metajit;
using namespace metajit::test;

using Bits = KnownBits::Bits;

uint64_t rand64() {
  return (uint64_t(rand()) << 32) | rand();
}

const int num_examples = 1000000;

std::pair<uint64_t, Bits> random_value_and_bits(Type type) {
  // half the time produce a constant
  if (rand() % 2 == 0) {
    uint64_t value = rand64();
    Bits bits = Bits::constant(type, value);
    return {value & type_mask(type), bits};
  }
  uint64_t mask = rand64();
  uint64_t concrete_value = rand64() & type_mask(type);
  uint64_t value = concrete_value & mask;
  Bits bits(type, mask, value);
  assert(bits.matches_const(concrete_value));
  return {concrete_value, bits};
}

void test_random(unittest::Suite& suite) {
  suite.test("random").run([]() {
    for (int i = 0; i < num_examples; i++) {
      auto [value_a, bits_a] = random_value_and_bits(Type::Int64);
      auto [value_b, bits_b] = random_value_and_bits(Type::Int64);

      unittest_assert ((bits_a & bits_b).matches_const(value_a & value_b));
      unittest_assert ((bits_a | bits_b).matches_const(value_a | value_b));
      unittest_assert ((bits_a ^ bits_b).matches_const(value_a ^ value_b));
      unittest_assert ((bits_a * bits_b).matches_const(value_a * value_b));
      if (value_b != 0) {
        unittest_assert (bits_a.div_u(bits_b).matches_const(value_a / value_b));
        unittest_assert (bits_a.div_s(bits_b).matches_const((int64_t)value_a / (int64_t)value_b));
        unittest_assert (bits_a.mod_u(bits_b).matches_const(value_a % value_b));
        unittest_assert (bits_a.mod_s(bits_b).matches_const((int64_t)value_a % (int64_t)value_b));
      }
      unittest_assert (bits_a.eq(bits_b).matches_const(value_a == value_b));
      unittest_assert (bits_a.lt_s(bits_b).matches_const((int64_t)value_a < (int64_t)value_b));
      unittest_assert (bits_a.lt_u(bits_b).matches_const(value_a < value_b));

      auto [value_bool, bits_bool] = random_value_and_bits(Type::Bool);
      unittest_assert (bits_bool.select(bits_a, bits_b).matches_const(value_bool ? value_a : value_b));
    }
  });
}

void test_random_add(unittest::Suite& suite) {
  suite.test("random_add").run([]() {
    for (int i = 0; i < num_examples; i++) {
      auto [value_a, bits_a] = random_value_and_bits(Type::Int64);
      auto [value_b, bits_b] = random_value_and_bits(Type::Int64);
      Bits c = bits_a + bits_b;
      unittest_assert (c.matches_const(value_a + value_b));
      if (bits_a.is_const() && bits_b.is_const()) {
        unittest_assert (c.is_const());
      }
    }
  });
}

void test_random_sub(unittest::Suite& suite) {
  suite.test("random_sub").run([]() {
    for (int i = 0; i < num_examples; i++) {
      auto [value_a, bits_a] = random_value_and_bits(Type::Int64);
      auto [value_b, bits_b] = random_value_and_bits(Type::Int64);
      Bits c = bits_a - bits_b;
      unittest_assert (c.matches_const(value_a - value_b));
      if (bits_a.is_const() && bits_b.is_const()) {
        unittest_assert (c.is_const());
      }
    }
  });
}

void test_random_shifts(unittest::Suite& suite) {
  suite.test("random_shifts").run([]() {
    for (int i = 0; i < num_examples; i++) {
      auto [value, bits] = random_value_and_bits(Type::Int64);
      uint64_t shift = rand() % 64;
      Bits shift_bits = Bits::constant(Type::Int64, shift);

      unittest_assert (bits.shr_u(shift_bits).matches_const(value >> shift));
      unittest_assert (bits.shr_s(shift_bits).matches_const((int64_t)value >> shift));
      unittest_assert (bits.shl(shift_bits).matches_const(value << shift));
    }
  });
}

void test_random_resize(unittest::Suite& suite) {
  suite.test("random_resize").run([]() {
    for (int i = 0; i < num_examples; i++) {
      auto [value, bits] = random_value_and_bits(Type::Int32);
      Bits bits_u64 = bits.resize_u(Type::Int64);
      Bits bits_s64 = bits.resize_s(Type::Int64);

      unittest_assert (bits_u64.matches_const(value));
      unittest_assert (bits_s64.matches_const((int64_t)(int32_t)value));
    }
  });
}

void test_add_example(unittest::Suite& suite) {
  suite.test("add_example").run([]() {
    Bits a = Bits(Type::Int64, 0b1011011011, 0b0010010010); // ?10?10?10
    Bits b = Bits(Type::Int64, 0b000111111, 0b000111000); // ???111000
    Bits c = a + b; //?01?10
    unittest_assert (c.mask == 0b11011);
    unittest_assert (c.value == 0b01010);

    a = Bits(Type::Int64, 0b111, 0); // alignment scenario
    b = Bits::constant(Type::Int64, 8);
    c = a + b;
    unittest_assert (c.mask == 0b111);
    unittest_assert (c.value == 0b0);
  });
}

void test_sub_example(unittest::Suite& suite) {
  suite.test("sub_example").run([]() {
    Bits a = Bits(Type::Int64, 0b1011011011, 0b0010010010); // ?10?10?10
    Bits b = Bits(Type::Int64, 0b000111111, 0b000111000); // ???111000
    Bits c = a - b; //?11?10
    unittest_assert (c.mask == 0b11011);
    unittest_assert (c.value == 0b11010);

    a = Bits(Type::Int64, 0b111, 0); // alignment scenario
    b = Bits::constant(Type::Int64, 8);
    c = a - b;
    unittest_assert (c.mask == 0b111);
    unittest_assert (c.value == 0b0);
  });
}

void test_idempotent_conditions(unittest::Suite& suite) {
  suite.test("idempotent_conditions").run([]() {
    for (int i = 0; i < num_examples; i++) {
      auto [value_a, bits_a] = random_value_and_bits(Type::Int8);
      auto [value_b, bits_b] = random_value_and_bits(Type::Int8);
      if (bits_a.and_idempotent_condition(bits_b)) {
        unittest_assert ((value_a & value_b) == value_a);
      }
      if (bits_b.and_idempotent_condition(bits_a)) {
        unittest_assert ((value_a & value_b) == value_b);
      }
      if (bits_a.or_idempotent_condition(bits_b)) {
        unittest_assert ((value_a | value_b) == value_a);
      }
      if (bits_b.or_idempotent_condition(bits_a)) {
        unittest_assert ((value_a | value_b) == value_b);
      }
    }
  });
}

void test_usedbits_shr_s_bug(unittest::Suite& suite) {
  suite.test("usedbits_shr_s_bug").run([]() {
    using Bits = UsedBits::Bits;
    Bits result(Type::Int32, 0xff000000);
    uint64_t used_bits_arg_shr_s = result.shr_s_arg_0(16);
    Bits arg(Type::Int32, used_bits_arg_shr_s);
    unittest_assert (used_bits_arg_shr_s == 0x80000000); // sign bit is needed
  });
}

void test_usedbits_shr(unittest::Suite& suite) {
  using Bits = UsedBits::Bits;
  suite.test("usedbits_shr").run([]() {
    for (int i = 0; i < num_examples; i++) {
      uint8_t x = rand() & 0xff;
      uint8_t y_extra_bits = rand() & 0xff;
      uint8_t shift = rand() & 7;
      Bits result(Type::Int8, rand() & 0xff);
      uint8_t used_bits_arg_shr_s = result.shr_s_arg_0(shift);
      uint8_t y = (x & used_bits_arg_shr_s) | (y_extra_bits & ~used_bits_arg_shr_s);
      unittest_assert ((x & used_bits_arg_shr_s) == (y & used_bits_arg_shr_s));
      unittest_assert (((int8_t(x) >> int8_t(shift)) & result.used) ==
                       ((int8_t(y) >> int8_t(shift)) & result.used));

      uint8_t used_bits_arg_shr_u = result.shr_u_arg_0(shift);
      y = (x & used_bits_arg_shr_u) | (y_extra_bits & ~used_bits_arg_shr_u);
      unittest_assert ((x & used_bits_arg_shr_u) == (y & used_bits_arg_shr_u));
      unittest_assert (((uint8_t(x) >> uint8_t(shift)) & result.used) ==
                       ((uint8_t(y) >> uint8_t(shift)) & result.used));
    }
  });
}

int main(int argc, char** argv) {
  unittest::Suite suite(argc, argv);

  suite.test("symbol_and_poison_have_unknown_bits").run([]() {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    NameMap<Bits> values(&section);
    auto check_unknown = [&](Value* value) {
      Bits bits = Bits::at(values, value);
      unittest_assert(bits.type == value->type());
      unittest_assert(bits.mask == 0);
      unittest_assert(bits.value == 0);
      unittest_assert(!bits.is_const());
    };
    check_unknown(builder.build_symbol(Type::Ptr, "target"));
    for (Type type : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64, Type::Ptr}) {
      check_unknown(builder.build_poison(type));
    }
  });

  suite.test("division_and_remainder_by_zero_are_unknown").run([]() {
    for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
      Bits zero = Bits::constant(type, 0);
      for (Bits numerator : {zero, Bits::constant(type, 7),
                             Bits::constant(type, type_mask(type)), Bits(type, 0, 0)}) {
        for (Bits result : {numerator.div_u(zero), numerator.div_s(zero),
                            numerator.mod_u(zero), numerator.mod_s(zero)}) {
          unittest_assert(result.type == type);
          unittest_assert(result.mask == 0);
          unittest_assert(result.value == 0);
          unittest_assert(!result.is_const());
        }
      }
    }
  });

  suite.test("signed_division_and_remainder_overflow_are_unknown").run([]() {
    for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
      uint64_t min_value = uint64_t(1) << (type_width(type) - 1);
      Bits minimum = Bits::constant(type, min_value);
      Bits minus_one = Bits::constant(type, type_mask(type));
      for (Bits result : {minimum.div_s(minus_one), minimum.mod_s(minus_one)}) {
        unittest_assert(result.type == type);
        unittest_assert(result.mask == 0);
        unittest_assert(result.value == 0);
        unittest_assert(!result.is_const());
      }
      Bits one = Bits::constant(type, 1);
      unittest_assert(minimum.div_s(one) == minimum);
      unittest_assert(minimum.mod_s(one) == Bits::constant(type, 0));
      Bits near_minimum = Bits::constant(type, min_value + 1);
      unittest_assert(near_minimum.div_s(minus_one) == Bits::constant(type, min_value - 1));
      unittest_assert(near_minimum.mod_s(minus_one) == Bits::constant(type, 0));
    }
  });

  suite.test("oversized_shifts_are_unknown").run([]() {
    for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
      size_t width = type_width(type);
      for (Bits operand : {Bits::constant(type, 0), Bits::constant(type, type_mask(type)),
                           Bits(type, 1, 1)}) {
        for (size_t shift : {width, width + 1, size_t(64), size_t(128), SIZE_MAX}) {
          Bits count = Bits::constant(type, shift);
          for (Bits result : {operand.shl(shift), operand.shr_u(shift), operand.shr_s(shift),
                              operand.shl(count), operand.shr_u(count), operand.shr_s(count)}) {
            unittest_assert(result.type == type);
            unittest_assert(result.mask == 0);
            unittest_assert(result.value == 0);
            unittest_assert(!result.is_const());
          }
        }
        unittest_assert(operand.shl(0) == operand);
        unittest_assert(operand.shr_u(0) == operand);
        unittest_assert(operand.shr_s(0) == operand);
      }
      Bits one = Bits::constant(type, 1);
      Bits ones = Bits::constant(type, type_mask(type));
      unittest_assert(one.shl(width - 1) == Bits::constant(type, uint64_t(1) << (width - 1)));
      unittest_assert(ones.shr_u(width - 1) == one);
      unittest_assert(ones.shr_s(width - 1) == ones);
    }
  });

  suite.test("usedbits_signed_right_shift_boundaries").run([]() {
    for (Type type : {Type::Int64, Type::Int32, Type::Int16, Type::Int8}) {
      size_t width = type_width(type);
      for (uint64_t used : {uint64_t(0), uint64_t(1), uint64_t(1) << (width - 1), type_mask(type)}) {
        UsedBits::Bits bits(type, used);
        unittest_assert(bits.shr_s_arg_0(0) == used);
        uint64_t expected = used ? uint64_t(1) << (width - 1) : 0;
        unittest_assert(bits.shr_s_arg_0(width - 1) == expected);
      }
    }
  });

  test_add_example(suite);
  test_sub_example(suite);
  test_random(suite);
  test_random_add(suite);
  test_random_sub(suite);
  test_random_shifts(suite);
  test_random_resize(suite);
  test_idempotent_conditions(suite);
  test_usedbits_shr_s_bug(suite);
  test_usedbits_shr(suite);

  return suite.finish();
}
