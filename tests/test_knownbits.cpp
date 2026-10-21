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

void test_variable_shifts() {
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    uint64_t width = type_width(type);
    uint64_t mask = type_mask(type);
    Bits ones = Bits::constant(type, mask);
    Bits zero = Bits::constant(type, 0);
    Bits count(type, mask & ~(width - 1), 0);
    unittest_assert(ones.shr_u(count) == Bits(type, 1, 1));
    uint64_t sign = uint64_t(1) << (width - 1);
    unittest_assert(ones.shl(count) == Bits(type, sign, sign));
    unittest_assert(ones.shr_s(count) == ones);
    unittest_assert(zero.shl(count) == zero);
    unittest_assert(zero.shr_u(count) == zero);
    unittest_assert(zero.shr_s(count) == zero);
    Bits unknown(type, 0, 0);
    unittest_assert(ones.shr_u(unknown) == ones.shr_u(count));
    unittest_assert(ones.shl(unknown) == ones.shl(count));
    unittest_assert(ones.shr_s(unknown) == ones);
    for (Bits operand : {Bits(type, 0, 0), Bits(type, sign | 1, 1), ones, zero}) {
      Bits zero_or_invalid(type, width - 1, 0);
      unittest_assert(operand.shl(zero_or_invalid) == operand);
      unittest_assert(operand.shr_u(zero_or_invalid) == operand);
      unittest_assert(operand.shr_s(zero_or_invalid) == operand);
      Bits maximum_or_invalid(type, width - 1, width - 1);
      unittest_assert(operand.shl(maximum_or_invalid) == operand.shl(width - 1));
      unittest_assert(operand.shr_u(maximum_or_invalid) == operand.shr_u(width - 1));
      unittest_assert(operand.shr_s(maximum_or_invalid) == operand.shr_s(width - 1));
      Bits invalid(type, width, width);
      unittest_assert(operand.shl(invalid) == unknown);
      unittest_assert(operand.shr_u(invalid) == unknown);
      unittest_assert(operand.shr_s(invalid) == unknown);
    }
  }
}

void test_random_variable_shifts() {
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    uint64_t width = type_width(type);
    for (int example = 0; example < num_examples / 100; example++) {
      auto [value, bits] = random_value_and_bits(type);
      uint64_t mask = rand64();
      Bits counts(type, mask, (rand64() % width) & mask);
      Bits left = bits.shl(counts);
      Bits right = bits.shr_u(counts);
      Bits signed_right = bits.shr_s(counts);
      Interpreter::Bits concrete = Interpreter::Bits::constant(type, value);
      for (uint64_t count = 0; count < width; count++) {
        if (!counts.matches_const(count)) {
          continue;
        }
        Interpreter::Bits amount = Interpreter::Bits::constant(type, count);
        unittest_assert(left.matches_const(concrete.shl(amount).value));
        unittest_assert(right.matches_const(concrete.shr_u(amount).value));
        unittest_assert(signed_right.matches_const(concrete.shr_s(amount).value));
      }
    }
  }
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

void test_usedbits_constant_shl() {
  using ConcreteBits = Interpreter::Bits;
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    uint64_t width = type_width(type);
    uint64_t mask = type_mask(type);
    for (uint64_t shift : {uint64_t(0), uint64_t(1), width / 2, width - 1,
                           width, width + 1, mask}) {
      for (uint64_t demanded : {uint64_t(0), uint64_t(1), uint64_t(1) << (width - 1),
                                mask, mask & uint64_t(0xaaaaaaaaaaaaaaaa)}) {
        Context context;
        Allocator allocator;
        Section section(context, allocator);
        Builder builder(&section);
        Block* entry = builder.build_block({Type::Ptr});
        builder.move_to_end(entry);
        Value* input = builder.build_load(entry->arg(0), type, LoadFlags::None, AliasingGroup(0), 0);
        Value* shifted = builder.build_shl(input, builder.build_const(type, shift));
        Value* result = builder.build_and(shifted, builder.build_const(type, demanded));
        builder.build_store(entry->arg(0), result, AliasingGroup(0), 8);
        builder.build_exit();
        section.order_blocks(BlockOrdering::Dominator);
        section.autoname();
        unittest_assert(!section.verify(std::cout));
        UsedBits used(&section);
        uint64_t input_used = used.at(input).used;
        uint64_t expected = 0;
        for (uint64_t bit = 0; bit < width; bit++) {
          if (shift < width && bit + shift < width &&
              (demanded & (uint64_t(1) << (bit + shift)))) {
            expected |= uint64_t(1) << bit;
          }
        }
        unittest_assert(input_used == expected);
        for (size_t sample = 0; sample < 100; sample++) {
          uint64_t original_value = rand64() & mask;
          uint64_t changed_value = (original_value & input_used) | (rand64() & mask & ~input_used);
          ConcreteBits amount = ConcreteBits::constant(type, shift);
          ConcreteBits original = ConcreteBits::constant(type, original_value).shl(amount);
          ConcreteBits changed = ConcreteBits::constant(type, changed_value).shl(amount);
          unittest_assert(original.is_poison == changed.is_poison);
          if (!original.is_poison) {
            unittest_assert(((original.value ^ changed.value) & demanded) == 0);
          }
        }
      }
    }
  }
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

void test_intersect_example(unittest::Suite& suite) {
  suite.test("intersect_example").run([]() {
    Bits a = Bits(Type::Int64, 0b10001111, 0b10001010); // 1???1010
    Bits b = Bits(Type::Int64, 0b11100011, 0b11000010); // 110???10
    auto c = a.intersect(b); // 110?1010;
    unittest_assert (c.has_value());
    unittest_assert (c.value().mask == 0b11101111);
    unittest_assert (c.value().value == 0b11001010);

    a = Bits(Type::Int64, 0b1, 0b0);
    b = Bits(Type::Int64, 0b1, 0b1);
    c = a.intersect(b);
    unittest_assert (not c.has_value());
  });
}

void test_random_intersect(unittest::Suite& suite) {
  suite.test("random_intersect").run([]() {
    for (int i = 0; i < num_examples; i++) {
      auto [value_a, bits_a] = random_value_and_bits(Type::Int8);
      auto [value_b, bits_b] = random_value_and_bits(Type::Int8);
      auto c = bits_a.intersect(bits_b);
      if (c.has_value()) {
        Bits bits_c = c.value();
        for (uint64_t j = 0; j < 256; j++) {
          if (bits_c.matches_const(j)) {
            unittest_assert (bits_a.matches_const(j));
            unittest_assert (bits_b.matches_const(j));
          }
        }
      } else {
        unittest_assert (!bits_a.matches_const(value_b));
        unittest_assert (!bits_b.matches_const(value_a));
      }
    }
  });
}

void test_and_backwards_example(unittest::Suite& suite) {
  suite.test("and_backwards_example").run([]() {
    Bits result = Bits(Type::Int8, 0b11010110, 0b10000100); // 10?0?10?
    Bits a = Bits(Type::Int8, 0b11111000, 0b11100000); // 11100???
    auto b = result.and_backwards(a); // 10???1??;
    unittest_assert (b.has_value());
    unittest_assert (b.value().mask == 0b11000100);
    unittest_assert (b.value().value == 0b10000100);

    result = Bits(Type::Int64, 0b1, 0b1); // ?...?1
    a = Bits(Type::Int64, 0b1, 0b0); // ?...?0
    b = result.and_backwards(a);
    unittest_assert (not b.has_value());
  });
}

void test_and_backwards_random(unittest::Suite& suite) {
  suite.test("and_backwards_random").run([]() {
    for (int i = 0; i < num_examples; i++) {
      auto [value_a, bits_a] = random_value_and_bits(Type::Int64);
      auto [value_b, bits_b] = random_value_and_bits(Type::Int64);
      auto result = bits_a & bits_b;
      auto better_b = result.and_backwards(bits_a);
      unittest_assert (better_b.has_value());
      unittest_assert (better_b.value().matches_const(value_b));
      unittest_assert (better_b.value().intersect(bits_b).has_value());
    }
  });
}

void test_lshift_backwards_example(unittest::Suite& suite) {
  suite.test("lshift_backwards_example").run([]() {
    Bits result = Bits(Type::Int8, 0b11010111, 0b10000000); // 10?0?000
    auto a = result.shl_backwards(3); // ???10?0?
    unittest_assert (a.has_value());
    unittest_assert (a.value().mask == 0b11010);
    unittest_assert (a.value().value == 0b10000);

    result = Bits(Type::Int8, 0b11010111, 0b10000111); // 10?0?111
    a = result.shl_backwards(3);
    unittest_assert (!a.has_value());
  });
}


void test_lshift_backwards_random(unittest::Suite& suite) {
  suite.test("lshift_backwards_random").run([]() {
    for (int i = 0; i < num_examples; i++) {
      auto [value_a, bits_a] = random_value_and_bits(Type::Int64);
      uint64_t shift = (rand() % 64);
      Bits result = bits_a.shl(shift);
      auto better_a = result.shl_backwards(shift);
      unittest_assert (better_a.has_value());
      unittest_assert (better_a.value().matches_const(value_a));
      unittest_assert (better_a.value().intersect(bits_a).has_value());
    }
  });
}

void test_lshift_backwards_boundaries(unittest::Suite& suite) {
  suite.test("lshift_backwards_boundaries").run([]() {
    for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
      size_t width = type_width(type);
      for (size_t shift = 0; shift < width; shift++) {
        uint64_t value = uint64_t(1) << shift;
        auto argument = Bits::constant(type, value).shl_backwards(shift);
        unittest_assert(argument.has_value());
        unittest_assert(argument->mask == (type_mask(type) >> shift));
        unittest_assert(argument->value == 1);
        if (shift != 0) {
          auto inconsistent = Bits::constant(type, value | (value >> 1)).shl_backwards(shift);
          unittest_assert(!inconsistent.has_value());
        }
      }
      for (size_t shift : {width, width + 1, size_t(64), size_t(128), SIZE_MAX}) {
        unittest_assert(!Bits::constant(type, 0).shl_backwards(shift).has_value());
      }
    }
  });
}


void test_or_backwards_exhaustive(unittest::Suite& suite) {
  suite.test("or_backwards_exhaustive").run([]() {
    for (uint64_t result_mask = 0; result_mask < 16; result_mask++) {
      for (uint64_t result_value = 0; result_value < 16; result_value++) {
        if (result_value & ~result_mask) continue;
        Bits result(Type::Int8, result_mask | 0xf0, result_value);
        for (uint64_t arg_mask = 0; arg_mask < 16; arg_mask++) {
          for (uint64_t arg_value = 0; arg_value < 16; arg_value++) {
            if (arg_value & ~arg_mask) continue;
            Bits argument(Type::Int8, arg_mask | 0xf0, arg_value);
            auto inferred = result.or_backwards(argument);
            bool possible = false;
            uint64_t common_ones = 0xff;
            uint64_t common_zeros = 0xff;
            for (uint64_t a = 0; a < 16; a++) {
              if (!argument.matches_const(a)) continue;
              for (uint64_t b = 0; b < 16; b++) {
                if (!result.matches_const(a | b)) continue;
                possible = true;
                common_ones &= b;
                common_zeros &= ~b;
              }
            }
            unittest_assert(inferred.has_value() == possible);
            if (possible) {
              unittest_assert(inferred->mask == (common_ones | common_zeros));
              unittest_assert(inferred->value == common_ones);
            }
          }
        }
      }
    }
  });
}

void test_or_backwards_random(unittest::Suite& suite) {
  suite.test("or_backwards_random").run([]() {
    for (int i = 0; i < num_examples; i++) {
      auto [value_a, bits_a] = random_value_and_bits(Type::Int64);
      auto [value_b, bits_b] = random_value_and_bits(Type::Int64);
      Bits result(Type::Int64, rand64(), value_a | value_b);
      auto inferred_a = result.or_backwards(bits_b);
      auto inferred_b = result.or_backwards(bits_a);
      unittest_assert(inferred_a.has_value());
      unittest_assert(inferred_b.has_value());
      unittest_assert(inferred_a->matches_const(value_a));
      unittest_assert(inferred_b->matches_const(value_b));
    }
  });
}

void test_shr_u_backwards_boundaries(unittest::Suite& suite) {
  suite.test("shr_u_backwards_boundaries").run([]() {
    for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
      size_t width = type_width(type);
      for (size_t shift = 0; shift < width; shift++) {
        auto argument = Bits::constant(type, type_mask(type) >> shift).shr_u_backwards(shift);
        uint64_t expected = (type_mask(type) << shift) & type_mask(type);
        unittest_assert(argument.has_value());
        unittest_assert(argument->mask == expected);
        unittest_assert(argument->value == expected);
        auto partial = Bits(type, 1, 1).shr_u_backwards(shift);
        unittest_assert(partial.has_value());
        unittest_assert(partial->mask == (uint64_t(1) << shift));
        unittest_assert(partial->value == partial->mask);
        if (shift != 0) {
          uint64_t invalid = uint64_t(1) << (width - shift);
          unittest_assert(!Bits(type, invalid, invalid).shr_u_backwards(shift).has_value());
        }
      }
      for (size_t shift : {width, width + 1, size_t(64), size_t(128), SIZE_MAX}) {
        unittest_assert(!Bits::constant(type, 0).shr_u_backwards(shift).has_value());
      }
    }
    auto partial = Bits(Type::Int8, 0b11110101, 0b00010100).shr_u_backwards(3);
    unittest_assert(partial.has_value());
    unittest_assert(partial->mask == 0b10101000);
    unittest_assert(partial->value == 0b10100000);
  });
}

void test_shr_u_backwards_random(unittest::Suite& suite) {
  suite.test("shr_u_backwards_random").run([]() {
    for (int i = 0; i < num_examples; i++) {
      uint64_t value = rand64();
      size_t shift = rand() % 64;
      Bits result(Type::Int64, rand64(), value >> shift);
      auto argument = result.shr_u_backwards(shift);
      unittest_assert(argument.has_value());
      unittest_assert(argument->matches_const(value));
      unittest_assert((argument->mask & ((uint64_t(1) << shift) - 1)) == 0);
    }
  });
}

void test_mask_through_highest_bit() {
  unittest_assert(Bits::mask_through_highest_bit(0) == 0);
  unittest_assert(Bits::mask_through_highest_bit(UINT64_MAX) == UINT64_MAX);
  unittest_assert(Bits::mask_through_highest_bit(10) == 15);
  unittest_assert(Bits::mask_through_highest_bit(0x123456789abcdef0ULL) == 0x1fffffffffffffffULL);
  for (size_t bit = 0; bit < 64; bit++) {
    uint64_t power = uint64_t(1) << bit;
    uint64_t expected = power | (power - 1);
    unittest_assert(Bits::mask_through_highest_bit(power) == expected);
    unittest_assert(Bits::mask_through_highest_bit(power - 1) == power - 1);
    unittest_assert(Bits::mask_through_highest_bit(expected) == expected);
  }
}

void test_unsigned_bounds() {
  for (Type type : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Bits unknown(type, 0, 0);
    unittest_assert(unknown.umin() == 0);
    unittest_assert(unknown.umax() == type_mask(type));
    const uint64_t values[] = {0, 1, uint64_t(1) << (type_width(type) - 1), type_mask(type)};
    for (uint64_t value : values) {
      Bits fixed = Bits::constant(type, value);
      unittest_assert(fixed.umin() == value);
      unittest_assert(fixed.umax() == value);
    }
    Bits sparse(type, 0xaaaaaaaaaaaaaaaaULL, 0x8888888888888888ULL);
    unittest_assert(sparse.umin() == (0x8888888888888888ULL & type_mask(type)));
    unittest_assert(sparse.umax() == (0xddddddddddddddddULL & type_mask(type)));
  }
}

void test_unsigned_bound_conversions() {
  for (Type type : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    uint64_t maximum = type_mask(type);
    uint64_t high = uint64_t(1) << (type_width(type) - 1);
    Bits unknown(type, 0, 0);
    unittest_assert(Bits::unsigned_upper_bound(type, 0) == Bits::constant(type, 0));
    unittest_assert(Bits::unsigned_upper_bound(type, maximum) == unknown);
    unittest_assert(Bits::unsigned_upper_bound(type, high - 1) == Bits(type, high, 0));
    unittest_assert(Bits::unsigned_upper_bound(type, high) == unknown);
    unittest_assert(Bits::unsigned_lower_bound(type, 0) == unknown);
    unittest_assert(Bits::unsigned_lower_bound(type, maximum) == Bits::constant(type, maximum));
    unittest_assert(Bits::unsigned_lower_bound(type, high) == Bits(type, high, high));
    unittest_assert(Bits::unsigned_lower_bound(type, high - 1) == unknown);
  }
}

void test_unsigned_bound_conversions_exhaustive() {
  for (uint64_t bound = 0; bound < 256; bound++) {
    uint64_t upper_ones = 255;
    uint64_t upper_any = 0;
    uint64_t lower_ones = 255;
    uint64_t lower_any = 0;
    for (uint64_t value = 0; value < 256; value++) {
      if (value <= bound) {
        upper_ones &= value;
        upper_any |= value;
      }
      if (value >= bound) {
        lower_ones &= value;
        lower_any |= value;
      }
    }
    unittest_assert(Bits::unsigned_upper_bound(Type::Int8, bound) ==
                    Bits(Type::Int8, upper_ones | ~upper_any, upper_ones));
    unittest_assert(Bits::unsigned_lower_bound(Type::Int8, bound) ==
                    Bits(Type::Int8, lower_ones | ~lower_any, lower_ones));
  }
}

void test_unsigned_remainder_bounds() {
  struct Bound {
    uint64_t divisor;
    uint64_t unknown;
  };
  const Bound bounds[] = {
    {1, 0}, {2, 1}, {3, 3}, {4, 3}, {5, 7}, {7, 7}, {8, 7}, {9, 15},
    {127, 127}, {128, 127}, {129, 255}, {255, 255}, {256, 255}, {257, 511},
    {(uint64_t(1) << 63) - 1, (uint64_t(1) << 63) - 1},
    {uint64_t(1) << 63, (uint64_t(1) << 63) - 1},
    {(uint64_t(1) << 63) + 1, ~uint64_t(0)}, {~uint64_t(0), ~uint64_t(0)}
  };
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Bits unknown(type, 0, 0);
    for (const Bound& bound : bounds) {
      if (bound.divisor > type_mask(type)) {
        continue;
      }
      Bits result = unknown.mod_u(Bits::constant(type, bound.divisor));
      unittest_assert(result == Bits(type, ~bound.unknown, 0));
    }
    Bits zero = Bits::constant(type, 0);
    unittest_assert(unknown.mod_u(zero) == unknown);
    unittest_assert(zero.mod_u(zero) == unknown);
    unittest_assert(Bits::constant(type, 37).mod_u(Bits::constant(type, 5)) == Bits::constant(type, 2));
  }
}

void test_unsigned_remainder_power_of_two_bits() {
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Bits input(type, 0xaaaaaaaaaaaaaaaaULL, 0x8888888888888888ULL);
    for (size_t shift = 0; shift < type_width(type); shift++) {
      uint64_t low_mask = (uint64_t(1) << shift) - 1;
      Bits expected(type, input.mask | ~low_mask, input.value & low_mask);
      unittest_assert(input.mod_u(Bits::constant(type, uint64_t(1) << shift)) == expected);
    }
  }
}

void test_unsigned_remainder_soundness_exhaustive() {
  for (uint64_t mask = 0; mask < 256; mask++) {
    for (uint64_t value = 0; value < 256; value++) {
      if (value & ~mask) {
        continue;
      }
      Bits input(Type::Int8, mask, value);
      std::vector<uint64_t> concrete_values;
      for (uint64_t concrete = 0; concrete < 256; concrete++) {
        if (input.matches_const(concrete)) {
          concrete_values.push_back(concrete);
        }
      }
      for (uint64_t divisor = 1; divisor < 256; divisor++) {
        Bits result = input.mod_u(Bits::constant(Type::Int8, divisor));
        unittest_assert(result.type == Type::Int8);
        for (uint64_t concrete : concrete_values) {
          unittest_assert(result.matches_const(concrete % divisor));
        }
      }
    }
  }
}

void test_unsigned_remainder_soundness_wide() {
  for (Type type : {Type::Int16, Type::Int32, Type::Int64}) {
    uint64_t high = uint64_t(1) << (type_width(type) - 1);
    for (int i = 0; i < 10000; i++) {
      uint64_t concrete = (rand64() ^ ((i & 1) ? high : 0)) & type_mask(type);
      uint64_t divisor = (rand64() ^ ((i & 2) ? high : 0)) & type_mask(type);
      if (divisor == 0) {
        divisor = 1;
      }
      Bits input(type, rand64() | ((i & 4) ? high : 0), concrete);
      unittest_assert(input.mod_u(Bits::constant(type, divisor)).matches_const(concrete % divisor));
    }
  }
}

void test_unsigned_remainder_simplify() {
  Context context;
  Allocator allocator;
  std::istringstream stream(R"(section {
b0(%0: Ptr, %1: Int64):
  %2 = ModU %1, 3:Int64
  %3 = LtU %2, 4:Int64
  %4 = And %1, 18446744073709551584:Int64
  %5 = ModU %4, 32:Int64
  Store %0, %3, aliasing=0, offset=0
  Store %0, %5, aliasing=0, offset=8
  Exit
}
)");
  Section* section = SectionReader<Builder>::read_section(context, allocator, stream);
  std::vector<StoreInst*> outputs;
  for (Inst* inst : *section->entry()) {
    if (dynmatch(StoreInst, store, inst)) {
      outputs.push_back(store);
    }
  }
  section->order_blocks(BlockOrdering::Dominator);
  Simplify::run(section, 10);
  unittest_assert(!section->verify(std::cout));
  unittest_assert(dynamic_cast<Const*>(outputs.at(0)->value()));
  unittest_assert(((Const*) outputs.at(0)->value())->value() == 1);
  unittest_assert(dynamic_cast<Const*>(outputs.at(1)->value()));
  unittest_assert(((Const*) outputs.at(1)->value())->value() == 0);
  delete section;
}

void test_unsigned_comparison_bounds() {
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    uint64_t high = uint64_t(1) << (type_width(type) - 1);
    Bits low(type, high, 0);
    Bits upper(type, high, high);
    Bits unknown(type, 0, 0);
    Bits zero = Bits::constant(type, 0);
    Bits maximum = Bits::constant(type, type_mask(type));
    Bits boundary = Bits::constant(type, high - 1);
    Bits even(type, 1, 0);
    struct Comparison {
      Bits a;
      Bits b;
      int expected;
    };
    const Comparison cases[] = {
      {low, upper, 1}, {upper, low, 0}, {unknown, zero, 0},
      {maximum, unknown, 0}, {zero, unknown, -1}, {unknown, maximum, -1},
      {low, boundary, -1}, {boundary, low, 0}, {unknown, unknown, -1},
      {even, maximum, 1}, {maximum, even, 0},
      {zero, zero, 0}, {maximum, maximum, 0}, {zero, maximum, 1}
    };
    for (const Comparison& comparison : cases) {
      Bits result = comparison.a.lt_u(comparison.b);
      unittest_assert(result.type == Type::Bool);
      if (comparison.expected < 0) {
        unittest_assert(!result.is_const());
      } else {
        unittest_assert(result.is_const());
        unittest_assert(result.value == uint64_t(comparison.expected));
      }
    }
  }
}

void test_signed_comparison_bounds() {
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    uint64_t high = uint64_t(1) << (type_width(type) - 1);
    Bits nonnegative(type, high, 0);
    Bits negative(type, high, high);
    Bits unknown(type, 0, 0);
    Bits small_positive(type, type_mask(type) & ~uint64_t(3), 0);
    Bits small_negative(type, type_mask(type) & ~uint64_t(3), type_mask(type) & ~uint64_t(3));
    Bits even(type, 1, 0);
    Bits zero = Bits::constant(type, 0);
    Bits minimum = Bits::constant(type, high);
    Bits maximum = Bits::constant(type, high - 1);
    struct Comparison {
      Bits a;
      Bits b;
      int expected;
    };
    const Comparison cases[] = {
      {negative, nonnegative, 1}, {nonnegative, negative, 0},
      {small_positive, Bits::constant(type, 32), 1},
      {Bits::constant(type, 32), small_positive, 0},
      {small_negative, Bits::constant(type, type_mask(type) - 4), 0},
      {Bits::constant(type, type_mask(type) - 4), small_negative, 1},
      {small_positive, Bits::constant(type, 3), -1},
      {small_negative, Bits::constant(type, type_mask(type)), -1},
      {negative, negative, -1}, {nonnegative, nonnegative, -1},
      {unknown, unknown, -1}, {unknown, zero, -1},
      {unknown, minimum, 0}, {maximum, unknown, 0},
      {minimum, unknown, -1}, {unknown, maximum, -1},
      {even, maximum, 1}, {maximum, even, 0},
      {zero, zero, 0}, {minimum, minimum, 0}, {maximum, maximum, 0},
      {minimum, maximum, 1}, {maximum, minimum, 0}
    };
    for (const Comparison& comparison : cases) {
      Bits result = comparison.a.lt_s(comparison.b);
      unittest_assert(result.type == Type::Bool);
      if (comparison.expected < 0) {
        unittest_assert(!result.is_const());
      } else {
        unittest_assert(result.is_const());
        unittest_assert(result.value == uint64_t(comparison.expected));
      }
    }
  }
}

void test_signed_comparison_soundness_exhaustive() {
  struct State {
    Bits bits;
    int minimum;
    int maximum;
  };
  std::vector<State> states;
  for (uint64_t mask = 0; mask < 256; mask++) {
    for (uint64_t value = 0; value < 256; value++) {
      if (value & ~mask) {
        continue;
      }
      Bits bits(Type::Int8, mask, value);
      int minimum = 128;
      int maximum = -129;
      for (int concrete = 0; concrete < 256; concrete++) {
        if (bits.matches_const(concrete)) {
          int signed_value = concrete < 128 ? concrete : concrete - 256;
          minimum = std::min(minimum, signed_value);
          maximum = std::max(maximum, signed_value);
        }
      }
      states.push_back({bits, minimum, maximum});
    }
  }
  unittest_assert(states.size() == 6561);
  for (const State& a : states) {
    for (const State& b : states) {
      Bits result = a.bits.lt_s(b.bits);
      unittest_assert(result.type == Type::Bool);
      unittest_assert(result.matches_const(a.minimum < b.maximum));
      unittest_assert(result.matches_const(a.maximum < b.minimum));
    }
  }
}

void test_signed_comparison_soundness_wide() {
  for (Type type : {Type::Int16, Type::Int32, Type::Int64}) {
    for (int i = 0; i < 10000; i++) {
      uint64_t high = uint64_t(1) << (type_width(type) - 1);
      uint64_t value_a = (rand64() ^ ((i & 1) ? high : 0)) & type_mask(type);
      uint64_t value_b = (rand64() ^ ((i & 2) ? high : 0)) & type_mask(type);
      uint64_t mask_a = rand64() | ((i & 4) ? high : 0);
      uint64_t mask_b = rand64() | ((i & 8) ? high : 0);
      Bits bits_a(type, mask_a, value_a);
      Bits bits_b(type, mask_b, value_b);
      bool expected = false;
      switch (type) {
        case Type::Int16: expected = int16_t(value_a) < int16_t(value_b); break;
        case Type::Int32: expected = int32_t(value_a) < int32_t(value_b); break;
        case Type::Int64: expected = int64_t(value_a) < int64_t(value_b); break;
        default: unittest_assert(false);
      }
      unittest_assert(bits_a.lt_s(bits_b).matches_const(expected));
    }
  }
}

void test_unsigned_comparison_after_resize() {
  Context context;
  Allocator allocator;
  std::istringstream stream(R"(section {
b0(%0: Ptr, %1: Int32):
  %2 = And %1, 16777215:Int32
  %3 = ResizeU %2, type=Int64
  %4 = LtU %3, 16777216:Int64
  %5 = LtU 16777216:Int64, %3
  %6 = LtU %3, 16777215:Int64
  Store %0, %4, aliasing=0, offset=0
  Store %0, %5, aliasing=0, offset=8
  Store %0, %6, aliasing=0, offset=16
  Exit
}
)");
  Section* section = SectionReader<Builder>::read_section(context, allocator, stream);
  std::vector<StoreInst*> outputs;
  for (Inst* inst : *section->entry()) {
    if (dynmatch(StoreInst, store, inst)) {
      outputs.push_back(store);
    }
  }
  section->order_blocks(BlockOrdering::Dominator);
  Simplify::run(section, 10);
  unittest_assert(!section->verify(std::cout));
  unittest_assert(dynamic_cast<Const*>(outputs.at(0)->value()));
  unittest_assert(((Const*) outputs.at(0)->value())->value() == 1);
  unittest_assert(dynamic_cast<Const*>(outputs.at(1)->value()));
  unittest_assert(((Const*) outputs.at(1)->value())->value() == 0);
  unittest_assert(dynamic_cast<LtUInst*>(outputs.at(2)->value()));
  delete section;
}

void test_signed_comparison_after_resize() {
  Context context;
  Allocator allocator;
  std::istringstream stream(R"(section {
b0(%0: Ptr, %1: Int8):
  %2 = And %1, 3:Int8
  %7 = ResizeU %2, type=Int32
  %3 = ResizeU %7, type=Int64
  %4 = LtS %3, 32:Int64
  %5 = LtS 32:Int64, %3
  %6 = LtS %3, 3:Int64
  Store %0, %4, aliasing=0, offset=0
  Store %0, %5, aliasing=0, offset=8
  Store %0, %6, aliasing=0, offset=16
  Exit
}
)");
  Section* section = SectionReader<Builder>::read_section(context, allocator, stream);
  std::vector<StoreInst*> outputs;
  for (Inst* inst : *section->entry()) {
    if (dynmatch(StoreInst, store, inst)) {
      outputs.push_back(store);
    }
  }
  section->order_blocks(BlockOrdering::Dominator);
  Simplify::run(section, 10);
  unittest_assert(!section->verify(std::cout));
  unittest_assert(dynamic_cast<Const*>(outputs.at(0)->value()));
  unittest_assert(((Const*) outputs.at(0)->value())->value() == 1);
  unittest_assert(dynamic_cast<Const*>(outputs.at(1)->value()));
  unittest_assert(((Const*) outputs.at(1)->value())->value() == 0);
  unittest_assert(dynamic_cast<LtSInst*>(outputs.at(2)->value()));
  delete section;
}

int main(int argc, char** argv) {
  unittest::Suite suite(argc, argv);

  suite.test("mask through highest bit").run(test_mask_through_highest_bit);
  suite.test("unsigned bounds").run(test_unsigned_bounds);
  suite.test("unsigned bound conversions").run(test_unsigned_bound_conversions);
  suite.test("unsigned bound conversions exhaustive").run(test_unsigned_bound_conversions_exhaustive);
  suite.test("unsigned remainder bounds").run(test_unsigned_remainder_bounds);
  suite.test("unsigned remainder power of two bits").run(test_unsigned_remainder_power_of_two_bits);
  suite.test("unsigned remainder soundness exhaustive").run(test_unsigned_remainder_soundness_exhaustive);
  suite.test("unsigned remainder soundness wide").run(test_unsigned_remainder_soundness_wide);
  suite.test("unsigned remainder simplify").run(test_unsigned_remainder_simplify);
  suite.test("unsigned comparison bounds").run(test_unsigned_comparison_bounds);
  suite.test("unsigned comparison after resize").run(test_unsigned_comparison_after_resize);
  suite.test("signed comparison bounds").run(test_signed_comparison_bounds);
  suite.test("signed comparison after resize").run(test_signed_comparison_after_resize);
  suite.test("signed comparison soundness exhaustive").run(test_signed_comparison_soundness_exhaustive);
  suite.test("signed comparison soundness wide").run(test_signed_comparison_soundness_wide);

  suite.test("usedbits_constant_shl").run(test_usedbits_constant_shl);

  using ConcreteBits = Interpreter::Bits;
  suite.test("usedbits_select_preserves_definedness").run([]() {
    for (uint64_t demanded : {uint64_t(255), uint64_t(1), uint64_t(128), uint64_t(0)}) {
      Context context;
      Allocator allocator;
      std::istringstream stream(
        "section {\n"
        "b0(%0: Ptr, %1: Int8, %2: Int8):\n"
        "  %3 = Load %0, type=Bool, flags={}, aliasing=0, offset=0\n"
        "  %4 = Select %3, %1, %2\n"
        "  %5 = And %4, " + std::to_string(demanded) + ":Int8\n"
        "  Store %0, %5, aliasing=0, offset=1\n"
        "  Exit\n"
        "}\n");
      std::unique_ptr<Section> section(SectionReader<>::read_section(context, allocator, stream));
      section->order_blocks(BlockOrdering::Dominator);
      unittest_assert(!section->verify(std::cout));
      Value* condition = *section->entry()->begin();
      UsedBits used(section.get());
      uint64_t condition_used = used.at(condition).used;
      for (bool poison_true : {false, true}) {
        ConcreteBits defined = ConcreteBits::constant(Type::Int8, 42);
        ConcreteBits poison = ConcreteBits::poison(Type::Int8);
        ConcreteBits a = poison_true ? poison : defined;
        ConcreteBits b = poison_true ? defined : poison;
        for (uint64_t cond : {uint64_t(0), uint64_t(1)}) {
          ConcreteBits original = ConcreteBits::constant(Type::Bool, cond).select(a, b);
          if (original.is_poison) continue;
          for (uint64_t extra : {uint64_t(0), uint64_t(1)}) {
            uint64_t changed_cond = (cond & condition_used) | (extra & ~condition_used);
            ConcreteBits changed = ConcreteBits::constant(Type::Bool, changed_cond).select(a, b);
            if (changed.is_poison) {
              std::cerr << "Select demanded=" << demanded << " condition mask=" << condition_used
                        << " original condition=" << cond << " changed condition=" << changed_cond
                        << " poison true arm=" << poison_true << "\n";
            }
            unittest_assert(!changed.is_poison);
            unittest_assert(((original.value ^ changed.value) & demanded) == 0);
          }
        }
      }
    }
  });

  struct UsedBitsOperation {
    const char* name;
    ConcreteBits (ConcreteBits::*evaluate)(const ConcreteBits&) const;
  };
  for (UsedBitsOperation operation : {
      UsedBitsOperation{"Shl", &ConcreteBits::shl},
      UsedBitsOperation{"ShrU", &ConcreteBits::shr_u},
      UsedBitsOperation{"ShrS", &ConcreteBits::shr_s},
      UsedBitsOperation{"DivU", &ConcreteBits::div_u},
      UsedBitsOperation{"DivS", &ConcreteBits::div_s},
      UsedBitsOperation{"ModU", &ConcreteBits::mod_u},
      UsedBitsOperation{"ModS", &ConcreteBits::mod_s}}) {
    for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
      suite.test(std::string("usedbits_preserves_defined_results_") + operation.name +
                 "_Int" + std::to_string(type_width(type))).run([operation, type]() {
        uint64_t width = type_width(type);
        uint64_t sign = uint64_t(1) << (width - 1);
        uint64_t mask = type_mask(type);
        for (uint64_t demanded : {mask, uint64_t(1), sign, mask >> 1, uint64_t(0)}) {
          Context context;
          Allocator allocator;
          std::string type_name = "Int" + std::to_string(width);
          std::istringstream stream(
            "section {\n"
            "b0(%0: Ptr):\n"
            "  %1 = Load %0, type=" + type_name + ", flags={}, aliasing=0, offset=0\n"
            "  %2 = Load %0, type=" + type_name + ", flags={}, aliasing=0, offset=8\n"
            "  %3 = " + operation.name + " %1, %2\n"
            "  %4 = And %3, " + std::to_string(demanded) + ":" + type_name + "\n"
            "  Store %0, %4, aliasing=0, offset=16\n"
            "  Exit\n"
            "}\n");
          std::unique_ptr<Section> section(SectionReader<>::read_section(context, allocator, stream));
          section->order_blocks(BlockOrdering::Dominator);
          unittest_assert(!section->verify(std::cout));
          auto inst = (*section->begin())->begin();
          Value* lhs = *inst++;
          Value* rhs = *inst++;
          UsedBits used(section.get());
          uint64_t lhs_used = used.at(lhs).used;
          uint64_t rhs_used = used.at(rhs).used;
          auto evaluate = [&](uint64_t a, uint64_t b) {
            return (ConcreteBits::constant(type, a).*operation.evaluate)(ConcreteBits::constant(type, b));
          };
          auto check = [&](uint64_t a, uint64_t b, uint64_t extra_a, uint64_t extra_b) {
            ConcreteBits original = evaluate(a, b);
            if (original.is_poison) return;
            uint64_t changed_a = ((a & lhs_used) | (extra_a & ~lhs_used)) & mask;
            uint64_t changed_b = ((b & rhs_used) | (extra_b & ~rhs_used)) & mask;
            ConcreteBits changed = evaluate(changed_a, changed_b);
            if (changed.is_poison || ((original.value ^ changed.value) & demanded) != 0) {
              std::cerr << operation.name << " " << type_name << " demanded=" << demanded
                        << " operand masks=" << lhs_used << "," << rhs_used
                        << " original=" << a << "," << b
                        << " changed=" << changed_a << "," << changed_b << "\n";
            }
            unittest_assert(!changed.is_poison);
            unittest_assert(((original.value ^ changed.value) & demanded) == 0);
          };
          std::vector<uint64_t> boundaries = {0, 1, 2, 7, width - 1, width, sign, sign + 1, mask};
          for (uint64_t a : boundaries) {
            for (uint64_t b : boundaries) {
              for (uint64_t extra_a : boundaries) {
                for (uint64_t extra_b : boundaries) {
                  check(a, b, extra_a, extra_b);
                }
              }
            }
          }
          for (size_t sample = 0; sample < 1000; sample++) {
            check(rand64() & mask, rand64() & mask, rand64(), rand64());
          }
        }
      });
    }
  }

  suite.test("comparison_result_types").run([]() {
    for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
      for (Bits a : {Bits(type, 0, 0), Bits(type, 1, 0), Bits::constant(type, 0)}) {
        for (Bits b : {Bits(type, 0, 0), Bits(type, 1, 1), Bits::constant(type, 1)}) {
          for (Bits result : {a.lt_s(b), a.lt_u(b)}) {
            unittest_assert(result.type == Type::Bool);
            if (a.is_const() && b.is_const()) {
              unittest_assert(result == Bits::constant(true));
            }
            unittest_assert((result & Bits::constant(false)) == Bits::constant(false));
          }
        }
      }
    }
  });

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

  suite.test("bit_reverse").run([]() {
    for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
      for (size_t bit = 0; bit < type_width(type); bit++) {
        uint64_t input_mask = uint64_t(1) << bit;
        uint64_t output_mask = uint64_t(1) << (type_width(type) - bit - 1);
        for (uint64_t value : {uint64_t(0), input_mask}) {
          Bits result = Bits(type, input_mask, value).bit_reverse();
          unittest_assert(result == Bits(type, output_mask, value ? output_mask : 0));
        }
        Context context;
        Allocator allocator;
        Section section(context, allocator);
        Builder builder(&section);
        Block* entry = builder.build_block({Type::Ptr});
        builder.move_to_end(entry);
        Value* input = builder.build_load(entry->arg(0), type, LoadFlags::None, AliasingGroup(0), 0);
        Value* masked = builder.build_and(input, builder.build_const(type, input_mask));
        Value* reversed = builder.build_bit_reverse(masked);
        Value* result = builder.build_and(reversed, builder.build_const(type, output_mask));
        builder.build_store(entry->arg(0), result, AliasingGroup(0), 8);
        builder.build_exit();
        section.order_blocks(BlockOrdering::Dominator);
        section.autoname();
        KnownBits known(&section);
        unittest_assert(known.at(reversed) == Bits(type, type_mask(type) ^ output_mask, 0));
        UsedBits used(&section);
        unittest_assert(used.at(masked).used == input_mask);
        unittest_assert(used.at(input).used == input_mask);
      }
    }
  });

  test_add_example(suite);
  test_sub_example(suite);
  test_random(suite);
  test_random_add(suite);
  test_random_sub(suite);
  test_random_shifts(suite);
  suite.test("variable shifts").run(test_variable_shifts);
  suite.test("random variable shifts").run(test_random_variable_shifts);
  test_random_resize(suite);
  test_idempotent_conditions(suite);
  test_usedbits_shr_s_bug(suite);
  test_usedbits_shr(suite);

  test_intersect_example(suite);
  test_random_intersect(suite);
  test_and_backwards_example(suite);
  test_and_backwards_random(suite);
  test_lshift_backwards_example(suite);
  test_lshift_backwards_boundaries(suite);
  test_lshift_backwards_random(suite);

  test_or_backwards_exhaustive(suite);
  test_or_backwards_random(suite);

  test_shr_u_backwards_boundaries(suite);
  test_shr_u_backwards_random(suite);

  return suite.finish();
}
