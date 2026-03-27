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

const std::string output_path = "tests/output/test_knownbits";

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

void test_random() {
  unittest::Test("random").run([]() {
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

void test_random_add() {
  unittest::Test("random_add").run([]() {
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

void test_random_sub() {
  unittest::Test("random_sub").run([]() {
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

void test_random_shifts() {
  unittest::Test("random_shifts").run([]() {
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

void test_random_resize() {
  unittest::Test("random_resize").run([]() {
    for (int i = 0; i < num_examples; i++) {
      auto [value, bits] = random_value_and_bits(Type::Int32);
      Bits bits_u64 = bits.resize_u(Type::Int64);
      Bits bits_s64 = bits.resize_s(Type::Int64);

      unittest_assert (bits_u64.matches_const(value));
      unittest_assert (bits_s64.matches_const((int64_t)(int32_t)value));
    }
  });
}

void test_add_example() {
  unittest::Test("add_example").run([]() {
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

void test_sub_example() {
  unittest::Test("sub_example").run([]() {
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

void test_idempotent_conditions() {
  unittest::Test("idempotent_conditions").run([]() {
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

void test_usedbits_shr_s_bug() {
  using Bits = UsedBits::Bits;
  Bits result(Type::Int32, 0xff000000);
  uint64_t used_bits_arg_shr_s = result.shr_s_arg_0(16);
  Bits arg(Type::Int32, used_bits_arg_shr_s);
  unittest_assert (used_bits_arg_shr_s == 0x80000000); // sign bit is needed
}

void test_usedbits_shr() {
  using Bits = UsedBits::Bits;
  unittest::Test("usedbits_shr").run([]() {
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

int main() {
  test_add_example();
  test_sub_example();
  test_random();
  test_random_add();
  test_random_sub();
  test_random_shifts();
  test_random_resize();
  test_idempotent_conditions();
  test_usedbits_shr_s_bug();
  test_usedbits_shr();
  return 0;
}
