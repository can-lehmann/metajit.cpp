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

void check_trace_simplify(const std::string& input,
                          std::initializer_list<size_t> chain_blocks,
                          const std::string& expected,
                          bool check_execution = true) {
  Context context;
  Allocator allocator;
  std::istringstream stream(input);
  std::unique_ptr<Section> section(SectionReader<>::read_section(context, allocator, stream));
  std::vector<Block*> blocks;
  for (Block* block : *section) {
    blocks.push_back(block);
  }
  Chain chain;
  for (size_t index : chain_blocks) {
    chain.add(blocks.at(index));
  }

  TestData data;
  for (Block* block : *section) {
    for (Inst* inst : *block) {
      if (dynmatch(LoadInst, load, inst)) {
        unittest_assert(load->ptr() == section->entry()->arg(0));
        unittest_assert(data.alloc_input(RandomRange(load->type())) == load->offset());
      } else if (dynmatch(StoreInst, store, inst)) {
        unittest_assert(store->ptr() == section->entry()->arg(0));
        unittest_assert(data.alloc_output(store->value()->type()) == store->offset());
      }
    }
  }

  unittest_assert(!section->verify(std::cout));
  Section original(context, allocator);
  Clone::run(section.get(), &original);
  SimplifyTrace::run(section.get(), &chain);
  std::stringstream ss;
  section->write(ss);
  if (ss.str() != expected) {
    std::cerr << "Expected:\n" << expected << "\n\nGot:\n" << ss.str() << std::endl;
  }
  unittest_assert(ss.str() == expected);

  unittest_assert(!section->verify(std::cout));
  if (check_execution) {
    check_opt_differential(&original, section.get(), data);
    check_codegen_differential("", section.get(), data);
  }
}

void test_generated_rewrite_guard_facts() {
  Context context;
  Allocator allocator;
  Section section(context, allocator);
  Builder builder(&section);
  Block* entry = builder.build_block({Type::Ptr, Type::Int8, Type::Int8});
  Block* taken = builder.build_block();
  Block* other = builder.build_block();
  builder.move_to_end(entry);
  Value* difference = builder.build_xor(entry->arg(1), entry->arg(2));
  Value* condition = builder.build_eq(difference, builder.build_const(Type::Int8, 0));
  Value* three = builder.build_const(Type::Int8, 3);
  Value* five = builder.build_const(Type::Int8, 5);
  StoreInst* before = builder.build_store(entry->arg(0), builder.build_select(condition, three, five), 0, 0);
  BranchInst* guard = builder.build_branch(condition, taken, other);
  builder.move_to_end(taken);
  StoreInst* after = builder.build_store(entry->arg(0), builder.build_select(condition, three, five), 0, 1);
  builder.build_exit();
  builder.move_to_end(other);
  builder.build_exit();
  Chain chain;
  chain.add(entry);
  chain.add(taken);
  SimplifyTrace::run(&section, &chain);
  SelectInst* earlier = dynamic_cast<SelectInst*>(before->value());
  unittest_assert(earlier && earlier->cond() == guard->cond());
  EqInst* rewritten = dynamic_cast<EqInst*>(guard->cond());
  unittest_assert(rewritten && rewritten->arg(0) == entry->arg(1) && rewritten->arg(1) == entry->arg(2));
  Const* result = dynamic_cast<Const*>(after->value());
  unittest_assert(result && result->value() == 3);
  unittest_assert(!section.verify(std::cout));
  for (uint64_t y : {uint64_t(1), uint64_t(2)}) {
    uint8_t output[2] = {42, 42};
    Interpreter interpreter(&section, {Interpreter::Bits::constant(output),
                            Interpreter::Bits::constant(Type::Int8, 1), Interpreter::Bits::constant(Type::Int8, y)});
    unittest_assert(interpreter.run() == Interpreter::Event::Exit);
    unittest_assert(output[0] == (y == 1 ? 3 : 5));
    unittest_assert(output[1] == (y == 1 ? 3 : 42));
  }
}

void test_unsigned_guard_known_bits() {
  const uint64_t bounds[] = {1, 2, 3, 7, 8, 9, 127, 128, 129, 255, 256,
                             16777215, 16777216, 16777217,
                             uint64_t(1) << 63, (uint64_t(1) << 63) + 1,
                             ~uint64_t(0)};
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    for (uint64_t bound : bounds) {
      if (bound > type_mask(type)) {
        continue;
      }
      uint64_t mask = bound - 1;
      for (size_t shift = 1; shift < 64; shift *= 2) {
        mask |= mask >> shift;
      }
      for (bool truth : {false, true}) {
        Context context;
        Allocator allocator;
        Section section(context, allocator);
        Builder builder(&section);
        Block* entry = builder.build_block({Type::Ptr, type});
        Block* success = builder.build_block();
        Block* failure = builder.build_block();
        builder.move_to_end(entry);
        Value* x = entry->arg(1);
        Value* cond = builder.build_lt_u(x, builder.build_const(type, bound));
        builder.build_branch(cond, truth ? success : failure, truth ? failure : success);
        builder.move_to_end(success);
        Value* masked = builder.build_and(x, builder.build_const(type, mask));
        StoreInst* store = builder.build_store(entry->arg(0), masked, AliasingGroup(0), 0);
        Value* smaller = builder.build_and(x, builder.build_const(type, mask >> 1));
        StoreInst* smaller_store = builder.build_store(entry->arg(0), smaller, AliasingGroup(0), 8);
        builder.build_exit();
        builder.move_to_end(failure);
        builder.build_exit();
        section.order_blocks(BlockOrdering::Dominator);
        Chain chain({entry, success});
        SimplifyTrace::run(&section, &chain);
        unittest_assert(!section.verify(std::cout));
        if (truth && bound == 1) {
          unittest_assert(dynamic_cast<Const*>(store->value()));
          unittest_assert(((Const*) store->value())->value() == 0);
        } else if (!truth && bound == type_mask(type)) {
          unittest_assert(dynamic_cast<Const*>(store->value()));
          unittest_assert(((Const*) store->value())->value() == bound);
        } else if (truth || mask == type_mask(type)) {
          unittest_assert(store->value() == x);
        } else {
          unittest_assert(store->value() != x);
        }
        if (bound > 1) {
          unittest_assert(smaller_store->value() != x);
        }
      }
    }
  }
}

void test_unsigned_guard_zero_contradiction() {
  Context context;
  Allocator allocator;
  Section section(context, allocator);
  Builder builder(&section);
  Block* entry = builder.build_block({Type::Int64});
  builder.move_to_end(entry);
  Value* cond = builder.build_lt_u(entry->arg(0), builder.build_const(Type::Int64, 0));
  builder.build_exit();
  section.order_blocks(BlockOrdering::Dominator);
  Chain chain({entry});
  SimplifyTrace simplify(&section, &chain);
  unittest_assert(!simplify.propagate_backwards(cond, KnownBits::Bits::constant(true)));
}

void test_unsigned_guard_remainder() {
  Context context;
  Allocator allocator;
  Section section(context, allocator);
  Builder builder(&section);
  Block* entry = builder.build_block({Type::Ptr});
  Block* success = builder.build_block();
  Block* failure = builder.build_block();
  builder.move_to_end(entry);
  TestData data(builder);
  Value* x = data.input(RandomRange(Type::Int64, 0, 16777218));
  Value* divisor = builder.build_const(Type::Int64, 16777216);
  Value* cond = builder.build_lt_u(x, divisor);
  builder.build_branch(cond, success, failure);
  builder.move_to_end(success);
  Value* remainder = builder.fold_mod_u(x, divisor);
  size_t offset = data.alloc_output(Type::Int64);
  StoreInst* store = builder.build_store(entry->arg(0), remainder, AliasingGroup(0), offset);
  builder.build_exit();
  builder.move_to_end(failure);
  builder.build_store(entry->arg(0), builder.build_const(Type::Int64, 0), AliasingGroup(0), offset);
  builder.build_exit();
  section.order_blocks(BlockOrdering::Dominator);
  Section original(context, allocator);
  Clone::run(&section, &original);
  Chain chain({entry, success});
  SimplifyTrace::run(&section, &chain);
  unittest_assert(store->value() == x);
  unittest_assert(!section.verify(std::cout));
  check_opt_differential(&original, &section, data);
}

enum class ComparisonKind { Unsigned, Signed };

struct ComparisonGuardFixture {
  Context context;
  Allocator allocator;
  Section section;
  Builder builder;
  Block* entry = nullptr;
  Value* cond = nullptr;
  Chain chain;
  SimplifyTrace simplify;
  bool accepted;

  Chain build_chain(ComparisonKind kind, Type type) {
    entry = builder.build_block({type, type});
    builder.move_to_end(entry);
    if (kind == ComparisonKind::Signed) {
      cond = builder.build_lt_s(entry->arg(0), entry->arg(1));
    } else {
      cond = builder.build_lt_u(entry->arg(0), entry->arg(1));
    }
    builder.build_exit();
    section.order_blocks(BlockOrdering::Dominator);
    return Chain({entry});
  }

  ComparisonGuardFixture(ComparisonKind kind, const KnownBits::Bits& x,
                         const KnownBits::Bits& y, bool truth):
    section(context, allocator), builder(&section),
    chain(build_chain(kind, x.type)), simplify(&section, &chain) {
    unittest_assert(x.type == y.type);
    unittest_assert(simplify.propagate_backwards(entry->arg(0), x));
    unittest_assert(simplify.propagate_backwards(entry->arg(1), y));
    accepted = simplify.propagate_backwards(cond, KnownBits::Bits::constant(truth));
  }
};

bool comparison_guard_accepts(ComparisonKind kind, const KnownBits::Bits& x,
                              const KnownBits::Bits& y, bool truth) {
  ComparisonGuardFixture fixture(kind, x, y, truth);
  return fixture.accepted;
}

bool comparison_guard_accepts(ComparisonKind kind, const KnownBits::Bits& x,
                              const KnownBits::Bits& y, bool truth,
                              size_t operand, const KnownBits::Bits& probe) {
  unittest_assert(operand < 2);
  unittest_assert(probe.type == x.type);
  ComparisonGuardFixture fixture(kind, x, y, truth);
  return fixture.accepted && fixture.simplify.propagate_backwards(fixture.entry->arg(operand), probe);
}

void test_unsigned_guard_nonconstant_bounds() {
  using Bits = KnownBits::Bits;
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    uint64_t high = uint64_t(1) << (type_width(type) - 1);
    uint64_t prefix = high | (high >> 1);
    Bits unknown(type, 0, 0);
    Bits low(type, prefix, 0);
    Bits high_value(type, prefix, prefix);
    for (uint64_t bit : {high, high >> 1}) {
      unittest_assert(!comparison_guard_accepts(ComparisonKind::Unsigned, unknown, low, true, 0, Bits(type, bit, bit)));
      unittest_assert(!comparison_guard_accepts(ComparisonKind::Unsigned, high_value, unknown, true, 1, Bits(type, bit, 0)));
      unittest_assert(!comparison_guard_accepts(ComparisonKind::Unsigned, unknown, high_value, false, 0, Bits(type, bit, 0)));
      unittest_assert(!comparison_guard_accepts(ComparisonKind::Unsigned, low, unknown, false, 1, Bits(type, bit, bit)));
    }
    uint64_t free_bit = high >> 2;
    unittest_assert(comparison_guard_accepts(ComparisonKind::Unsigned, unknown, low, true, 0, Bits(type, free_bit, free_bit)));
    unittest_assert(comparison_guard_accepts(ComparisonKind::Unsigned, high_value, unknown, true, 1, Bits(type, free_bit, 0)));
    unittest_assert(comparison_guard_accepts(ComparisonKind::Unsigned, unknown, high_value, false, 0, Bits(type, free_bit, 0)));
    unittest_assert(comparison_guard_accepts(ComparisonKind::Unsigned, low, unknown, false, 1, Bits(type, free_bit, free_bit)));
    unittest_assert(comparison_guard_accepts(ComparisonKind::Unsigned, unknown, low, true, 0, Bits(type, 1, 1)));
    unittest_assert(comparison_guard_accepts(ComparisonKind::Unsigned, high_value, unknown, true, 1, Bits(type, 1, 0)));
    Bits low_bit_set(type, prefix | 1, prefix | 1);
    Bits low_bit_clear(type, 1, 0);
    unittest_assert(comparison_guard_accepts(ComparisonKind::Unsigned, unknown, low_bit_set, false, 0, Bits(type, 1, 0)));
    unittest_assert(comparison_guard_accepts(ComparisonKind::Unsigned, low_bit_clear, unknown, false, 1, Bits(type, 1, 1)));
    unittest_assert(comparison_guard_accepts(ComparisonKind::Unsigned, unknown, unknown, true));
    unittest_assert(comparison_guard_accepts(ComparisonKind::Unsigned, unknown, unknown, false));
  }
}

void test_unsigned_guard_boundaries() {
  using Bits = KnownBits::Bits;
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    uint64_t mask = type_mask(type);
    uint64_t high = uint64_t(1) << (type_width(type) - 1);
    Bits unknown(type, 0, 0);
    Bits zero = Bits::constant(type, 0);
    Bits maximum = Bits::constant(type, mask);
    Bits low(type, high, 0);
    Bits high_value(type, high, high);
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Unsigned, maximum, unknown, true));
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Unsigned, unknown, zero, true));
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Unsigned, high_value, low, true));
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Unsigned, low, high_value, false));
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Unsigned, zero, zero, true));
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Unsigned, maximum, maximum, true));
    unittest_assert(comparison_guard_accepts(ComparisonKind::Unsigned, zero, zero, false));
    unittest_assert(comparison_guard_accepts(ComparisonKind::Unsigned, maximum, maximum, false));
    unittest_assert(comparison_guard_accepts(ComparisonKind::Unsigned, zero, maximum, true));
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Unsigned, zero, maximum, false));
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Unsigned, unknown, maximum, false, 0, Bits(type, 1, 0)));
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Unsigned, zero, unknown, false, 1, Bits(type, 1, 1)));
    Bits near_maximum(type, mask ^ 1, mask ^ 1);
    Bits near_zero(type, mask ^ 1, 0);
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Unsigned, near_maximum, unknown, true, 1, Bits(type, 1, 0)));
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Unsigned, unknown, near_zero, true, 0, Bits(type, 1, 1)));
  }
}

void test_signed_guard_boundaries() {
  using Bits = KnownBits::Bits;
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    uint64_t mask = type_mask(type);
    uint64_t high = uint64_t(1) << (type_width(type) - 1);
    Bits unknown(type, 0, 0);
    Bits minimum = Bits::constant(type, high);
    Bits maximum = Bits::constant(type, high - 1);
    Bits zero = Bits::constant(type, 0);
    Bits negative(type, high, high);
    Bits nonnegative(type, high, 0);
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Signed, maximum, unknown, true));
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Signed, unknown, minimum, true));
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Signed, nonnegative, negative, true));
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Signed, negative, nonnegative, false));
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Signed, minimum, minimum, true));
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Signed, maximum, maximum, true));
    unittest_assert(comparison_guard_accepts(ComparisonKind::Signed, minimum, minimum, false));
    unittest_assert(comparison_guard_accepts(ComparisonKind::Signed, maximum, maximum, false));
    unittest_assert(comparison_guard_accepts(ComparisonKind::Signed, minimum, maximum, true));
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Signed, minimum, maximum, false));
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Signed, unknown, zero, true, 0, nonnegative));
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Signed, unknown, zero, false, 0, negative));
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Signed, unknown, maximum, false, 0, Bits(type, 1, 0)));
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Signed, minimum, unknown, false, 1, Bits(type, 1, 1)));
    Bits near_maximum(type, mask ^ 1, (high - 1) ^ 1);
    Bits near_minimum(type, mask ^ 1, high);
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Signed, near_maximum, unknown, true, 1, Bits(type, 1, 0)));
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Signed, unknown, near_minimum, true, 0, Bits(type, 1, 1)));
  }
}

void test_signed_guard_nonconstant_bounds() {
  using Bits = KnownBits::Bits;
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    uint64_t high = uint64_t(1) << (type_width(type) - 1);
    uint64_t next = high >> 1;
    Bits unknown(type, 0, 0);
    Bits negative_low(type, high | next, high);
    Bits positive_high(type, high | next, next);
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Signed, unknown, negative_low, true, 0, Bits(type, high, 0)));
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Signed, unknown, negative_low, true, 0, Bits(type, next, next)));
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Signed, positive_high, unknown, true, 1, Bits(type, high, high)));
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Signed, positive_high, unknown, true, 1, Bits(type, next, 0)));
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Signed, unknown, positive_high, false, 0, Bits(type, high, high)));
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Signed, unknown, positive_high, false, 0, Bits(type, next, 0)));
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Signed, negative_low, unknown, false, 1, Bits(type, high, 0)));
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Signed, negative_low, unknown, false, 1, Bits(type, next, next)));
    unittest_assert(comparison_guard_accepts(ComparisonKind::Signed, unknown, negative_low, true, 0, Bits(type, next >> 1, next >> 1)));
    unittest_assert(comparison_guard_accepts(ComparisonKind::Signed, positive_high, unknown, true, 1, Bits(type, next >> 1, 0)));
    Bits nonnegative(type, high, 0);
    Bits four = Bits::constant(type, 4);
    unittest_assert(!comparison_guard_accepts(ComparisonKind::Signed, nonnegative, four, true, 0, Bits(type, 4, 4)));
    unittest_assert(comparison_guard_accepts(ComparisonKind::Signed, nonnegative, four, true, 0, Bits::constant(type, 3)));
    unittest_assert(comparison_guard_accepts(ComparisonKind::Signed, unknown, unknown, true));
    unittest_assert(comparison_guard_accepts(ComparisonKind::Signed, unknown, unknown, false));
  }
}

void test_signed_guard_known_bits() {
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    for (bool truth : {false, true}) {
      Context context;
      Allocator allocator;
      Section section(context, allocator);
      Builder builder(&section);
      Block* entry = builder.build_block({Type::Ptr, type});
      Block* success = builder.build_block();
      Block* failure = builder.build_block();
      builder.move_to_end(entry);
      Value* x = entry->arg(1);
      Value* cond = builder.build_lt_s(x, builder.build_const(type, 0));
      builder.build_branch(cond, truth ? success : failure, truth ? failure : success);
      builder.move_to_end(success);
      Value* sign = builder.build_shr_u(x, builder.build_const(type, type_width(type) - 1));
      StoreInst* store = builder.build_store(entry->arg(0), sign, AliasingGroup(0), 0);
      builder.build_exit();
      builder.move_to_end(failure);
      builder.build_exit();
      section.order_blocks(BlockOrdering::Dominator);
      Chain chain({entry, success});
      SimplifyTrace::run(&section, &chain);
      unittest_assert(!section.verify(std::cout));
      unittest_assert(dynamic_cast<Const*>(store->value()));
      unittest_assert(((Const*) store->value())->value() == uint64_t(truth));
    }
  }
}

bool signed_concrete_less(Type type, uint64_t a, uint64_t b) {
  switch (type) {
    case Type::Int8: return int8_t(a) < int8_t(b);
    case Type::Int16: return int16_t(a) < int16_t(b);
    case Type::Int32: return int32_t(a) < int32_t(b);
    case Type::Int64: return int64_t(a) < int64_t(b);
    default: unittest_assert(false); return false;
  }
}

void test_signed_guard_soundness() {
  using Bits = KnownBits::Bits;
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    uint64_t high = uint64_t(1) << (type_width(type) - 1);
    for (uint64_t free : {high | (high >> 1) | 1, (high >> 1) | 1}) {
      uint64_t mask = type_mask(type) ^ free;
      for (uint64_t x_base : {uint64_t(0), uint64_t(2), mask}) {
        for (uint64_t y_base : {uint64_t(0), uint64_t(2), mask}) {
          Bits x(type, mask, x_base);
          Bits y(type, mask, y_base);
          for (bool truth : {false, true}) {
            bool possible = false;
            for (uint64_t a = 0; a < 8; a++) {
              uint64_t x_value = x_base | ((((a & 1) ? high : 0) | ((a & 2) ? high >> 1 : 0) | ((a & 4) ? 1 : 0)) & free);
              for (uint64_t b = 0; b < 8; b++) {
                uint64_t y_value = y_base | ((((b & 1) ? high : 0) | ((b & 2) ? high >> 1 : 0) | ((b & 4) ? 1 : 0)) & free);
                if (signed_concrete_less(type, x_value, y_value) == truth) {
                  possible = true;
                  unittest_assert(comparison_guard_accepts(ComparisonKind::Signed, x, y, truth, 0, Bits::constant(type, x_value)));
                  unittest_assert(comparison_guard_accepts(ComparisonKind::Signed, x, y, truth, 1, Bits::constant(type, y_value)));
                }
              }
            }
            unittest_assert(comparison_guard_accepts(ComparisonKind::Signed, x, y, truth) == possible);
          }
        }
      }
    }
  }
}

int main(int argc, char** argv) {
  unittest::Suite suite(argc, argv);
  metajit::LLVMCodeGen::initilize_llvm_jit();

  suite.test("generated rewrite guard facts").run(test_generated_rewrite_guard_facts);
  suite.test("signed guard boundaries").run(test_signed_guard_boundaries);
  suite.test("signed guard nonconstant bounds").run(test_signed_guard_nonconstant_bounds);
  suite.test("signed guard known bits").run(test_signed_guard_known_bits);
  suite.test("signed guard soundness").run(test_signed_guard_soundness);
  suite.test("unsigned guard nonconstant bounds").run(test_unsigned_guard_nonconstant_bounds);
  suite.test("unsigned guard boundaries").run(test_unsigned_guard_boundaries);
  suite.test("unsigned guard known bits").run(test_unsigned_guard_known_bits);
  suite.test("unsigned guard zero contradiction").run(test_unsigned_guard_zero_contradiction);
  suite.test("unsigned guard remainder").run(test_unsigned_guard_remainder);

  suite.test("freeze_preserves_output_relationship").run([]() {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* entry = builder.build_block({Type::Ptr});
    builder.move_to_end(entry);
    Value* a = builder.build_and(builder.build_poison(Type::Int8), builder.build_const(Type::Int8, 1));
    Value* frozen = builder.build_freeze(a);
    AndInst* masked = builder.build_and(frozen, builder.build_const(Type::Int8, 2));
    StoreInst* frozen_store = builder.build_store(entry->arg(0), frozen, AliasingGroup(0), 0);
    StoreInst* masked_store = builder.build_store(entry->arg(0), masked, AliasingGroup(0), 1);
    builder.build_exit();
    section.order_blocks(BlockOrdering::Dominator);
    unittest_assert(!section.verify(std::cout));
    Chain chain({entry});
    SimplifyTrace::run(&section, &chain);
    unittest_assert(!section.verify(std::cout));
    unittest_assert(frozen_store->value() == frozen);
    unittest_assert(masked_store->value() == masked);
    unittest_assert(masked->arg(0) == frozen);
  });

  suite.test("symbolic_pointer_guard").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  %1 = Eq %0, @target:Ptr
  Branch %1, true_block=b1, false_block=b2
b1:
  %3 = Eq %0, @other:Ptr
  Store %0, %3, aliasing=0, offset=0
  Exit
b2:
  Exit
}
)";
    check_trace_simplify(ir, {0, 1}, ir, false);
  });

  suite.test("constant_zero_divisors").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  Branch 0:Bool, true_block=b1, false_block=b2
b1:
  %2 = DivU 7:Int64, 0:Int64
  %3 = DivS 7:Int64, 0:Int64
  %4 = ModU 7:Int64, 0:Int64
  %5 = ModS 7:Int64, 0:Int64
  %6 = Freeze %2
  %7 = Freeze %3
  %8 = Freeze %4
  %9 = Freeze %5
  Exit
b2:
  Exit
}
)";
    check_trace_simplify(ir, {1}, ir, false);
  });

  suite.test("invalid_constant_arithmetic_stays_unknown").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  %1 = DivS 9223372036854775808:Int64, 18446744073709551615:Int64
  %2 = ModS 9223372036854775808:Int64, 18446744073709551615:Int64
  %3 = Shl 1:Int64, 64:Int64
  %4 = ShrU 1:Int64, 64:Int64
  %5 = ShrS 1:Int64, 64:Int64
  %6 = Freeze %1
  %7 = Freeze %2
  %8 = Freeze %3
  %9 = Freeze %4
  %10 = Freeze %5
  Store %0, 0:Int64, aliasing=0, offset=0
  Exit
}
)";
    check_trace_simplify(ir, {0}, ir, false);
  });

  suite.test("freeze_poison").run([]() {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* entry = builder.build_block({Type::Ptr});
    builder.move_to_end(entry);
    builder.build_freeze(builder.build_poison(Type::Bool));
    builder.build_store(entry->arg(0), builder.build_const(Type::Bool, 0), AliasingGroup(0), 0);
    builder.build_exit();
    unittest_assert(!section.verify(std::cout));
    Section original(context, allocator);
    Clone::run(&section, &original);
    Chain chain({entry});
    SimplifyTrace::run(&section, &chain);
    unittest_assert(!section.verify(std::cout));
    std::stringstream result;
    section.write(result);
    unittest_assert(result.str() == R"(section {
b0(%0: Ptr):
  %1 = Freeze poison:Bool
  Store %0, 0:Bool, aliasing=0, offset=0
  Exit
}
)");
    TestData data;
    data.alloc_output(Type::Bool);
    check_opt_differential(&original, &section, data);
  });

  suite.test("const_prop_branch").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int64, flags={}, aliasing=0, offset=8
  Branch %1, true_block=b1, false_block=b2
b1:
  %4 = Select %1, %2, 0:Int64
  Store %0, %4, aliasing=0, offset=16
  Jump block=b2
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int64, flags={}, aliasing=0, offset=8
  Branch %1, true_block=b1, false_block=b2
b1:
  %4 = Select 1:Bool, %2, 0:Int64
  Store %0, %2, aliasing=0, offset=16
  Jump block=b2
b2:
  Exit
}
)");
  });

  suite.test("const_prop_eq_backwards").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Eq %1, 42:Int8
  Branch %2, true_block=b1, false_block=b2
b1:
  %4 = Add %1, 17:Int8
  Store %0, %4, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Eq %1, 42:Int8
  Branch %2, true_block=b1, false_block=b2
b1:
  %4 = Add 42:Int8, 17:Int8
  Store %0, 59:Int8, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)");
  });

  suite.test("const_prop_resize_x_backwards").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = ResizeX %1, type=Bool
  Branch %2, true_block=b1, false_block=b2
b1:
  %4 = And %1, 1:Int8
  Store %0, %4, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = ResizeX %1, type=Bool
  Branch %2, true_block=b1, false_block=b2
b1:
  %4 = And %1, 1:Int8
  Store %0, 1:Int8, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)");
  });

  suite.test("backwards_with_intersect").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Or %1, 6:Int8
  %3 = ResizeX %2, type=Bool
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %2, 7:Int8
  Store %0, %5, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Or %1, 6:Int8
  %3 = ResizeX %2, type=Bool
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %2, 7:Int8
  Store %0, 7:Int8, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)");
  });

  suite.test("backwards_and").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = And %1, 7:Int8
  %3 = Eq %2, 7:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %1, 6:Int8
  Store %0, %5, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = And %1, 7:Int8
  %3 = Eq %2, 7:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %1, 6:Int8
  Store %0, 6:Int8, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)");
  });

  suite.test("backwards_select").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Select %1, 4:Int8, 7:Int8
  %3 = Eq %2, 4:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, %1, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Select %1, 4:Int8, 7:Int8
  %3 = Eq %2, 4:Int8
  Branch %1, true_block=b1, false_block=b2
b1:
  Store %0, 1:Bool, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)");
  });

  suite.test("backwards_add").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Add %1, 1:Int8
  %3 = Eq %2, 4:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, %1, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Add %1, 1:Int8
  %3 = Eq %2, 4:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, 3:Int8, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)");
  });

  suite.test("backwards_shl").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Shl %1, 2:Int8
  %3 = Eq %2, 4:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %1, 15:Int8
  Store %0, %5, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Shl %1, 2:Int8
  %3 = Eq %2, 4:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %1, 15:Int8
  Store %0, 1:Int8, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)");
  });

  suite.test("backwards_resize_u").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = ResizeU %1, type=Int64
  %3 = Eq %2, 4:Int64
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, %1, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = ResizeU %1, type=Int64
  %3 = Eq %2, 4:Int64
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, 4:Int8, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)");
  });

  suite.test("backwards_xor").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Xor %1, 1:Bool
  %3 = Load %0, type=Int64, flags={}, aliasing=0, offset=8
  Branch %2, true_block=b1, false_block=b2
b1:
  Store %0, %1, aliasing=0, offset=16
  Jump block=b2
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Xor %1, 1:Bool
  %3 = Load %0, type=Int64, flags={}, aliasing=0, offset=8
  Branch %2, true_block=b1, false_block=b2
b1:
  Store %0, 0:Bool, aliasing=0, offset=16
  Jump block=b2
b2:
  Exit
}
)");
  });

  suite.test("eq_resizeu_bool").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = ResizeU %1, type=Int64
  %3 = Eq %2, 1:Int64
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, %1, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = ResizeU %1, type=Int64
  %3 = Eq %2, 1:Int64
  Branch %1, true_block=b1, false_block=b2
b1:
  Store %0, 1:Bool, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)");
  });

  suite.test("completed_true_continuation").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  Branch %1, true_block=b1, false_block=b2
b1:
  Store %0, %1, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  Branch %1, true_block=b1, false_block=b2
b1:
  Store %0, 1:Bool, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)");
  });

  suite.test("completed_false_continuation").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  Branch %1, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Store %0, %1, aliasing=0, offset=1
  Exit
}
)", {0, 2}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  Branch %1, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Store %0, 0:Bool, aliasing=0, offset=1
  Exit
}
)");
  });

  suite.test("empty_chain").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  Branch %1, true_block=b1, false_block=b2
b1:
  Store %0, %1, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)";
    check_trace_simplify(ir, {}, ir);
  });

  suite.test("single_block_chain").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = And %1, 0:Bool
  Store %0, %2, aliasing=0, offset=1
  Exit
}
)", {0}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = And %1, 0:Bool
  Store %0, 0:Bool, aliasing=0, offset=1
  Exit
}
)");
  });

  suite.test("stop_at_chain_end").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Bool, flags={}, aliasing=0, offset=1
  Branch %1, true_block=b1, false_block=b2
b1:
  Branch %2, true_block=b3, false_block=b2
b2:
  Exit
b3:
  Store %0, %1, aliasing=0, offset=2
  Store %0, %2, aliasing=0, offset=3
  Jump block=b4
b4:
  Exit
}
)";
    check_trace_simplify(ir, {0, 1}, ir);
  });

  suite.test("multiple_guards").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Bool, flags={}, aliasing=0, offset=1
  Branch %1, true_block=b1, false_block=b2
b1:
  Branch %2, true_block=b3, false_block=b2
b2:
  Exit
b3:
  Store %0, %1, aliasing=0, offset=2
  Store %0, %2, aliasing=0, offset=3
  Jump block=b4
b4:
  Exit
}
)", {0, 1, 3}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Bool, flags={}, aliasing=0, offset=1
  Branch %1, true_block=b1, false_block=b2
b1:
  Branch %2, true_block=b3, false_block=b2
b2:
  Exit
b3:
  Store %0, 1:Bool, aliasing=0, offset=2
  Store %0, 1:Bool, aliasing=0, offset=3
  Jump block=b4
b4:
  Exit
}
)");
  });

  suite.test("continue_across_jump").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Bool, flags={}, aliasing=0, offset=1
  Branch %1, true_block=b1, false_block=b2
b1:
  Jump block=b3
b2:
  Exit
b3:
  Store %0, %1, aliasing=0, offset=2
  Store %0, %2, aliasing=0, offset=3
  Jump block=b4
b4:
  Exit
}
)", {0, 1, 3}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Bool, flags={}, aliasing=0, offset=1
  Branch %1, true_block=b1, false_block=b2
b1:
  Jump block=b3
b2:
  Exit
b3:
  Store %0, 1:Bool, aliasing=0, offset=2
  Store %0, %2, aliasing=0, offset=3
  Jump block=b4
b4:
  Exit
}
)");
  });

  suite.test("chain_head_with_same_branch_targets").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  Branch %1, true_block=b1, false_block=b1
b1:
  Store %0, %1, aliasing=0, offset=1
  Exit
}
)";
    check_trace_simplify(ir, {1}, ir);
  });

  suite.test("chain_head_with_other_predecessor").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  Branch %1, true_block=b1, false_block=b2
b1:
  Jump block=b2
b2:
  Store %0, %1, aliasing=0, offset=1
  Exit
}
)";
    check_trace_simplify(ir, {2}, ir);
  });

  suite.test("nonadjacent_chain_blocks").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  Branch %1, true_block=b1, false_block=b2
b1:
  Jump block=b3
b2:
  Exit
b3:
  %5 = And %1, 0:Bool
  Store %0, %5, aliasing=0, offset=1
  Exit
}
)", {0, 1, 3}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  Branch %1, true_block=b1, false_block=b2
b1:
  Jump block=b3
b2:
  Exit
b3:
  %5 = And 1:Bool, 0:Bool
  Store %0, 0:Bool, aliasing=0, offset=1
  Exit
}
)");
  });

  suite.test("backwards_or_left").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Or %1, 8:Int8
  %3 = Eq %2, 13:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %1, 7:Int8
  Store %0, %5, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Or %1, 8:Int8
  %3 = Eq %2, 13:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %1, 7:Int8
  Store %0, 5:Int8, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)");
  });

  suite.test("backwards_or_right").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Or 8:Int8, %1
  %3 = Eq %2, 13:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %1, 7:Int8
  Store %0, %5, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Or 8:Int8, %1
  %3 = Eq %2, 13:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %1, 7:Int8
  Store %0, 5:Int8, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)");
  });

  suite.test("backwards_sub_left_wrap").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Sub %1, 1:Int8
  %3 = Eq %2, 255:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, %1, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Add %1, 255:Int8
  %3 = Sub %1, 1:Int8
  %4 = Eq %2, 255:Int8
  Branch %4, true_block=b1, false_block=b2
b1:
  Store %0, 0:Int8, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)");
  });

  suite.test("backwards_sub_right_wrap").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Sub 1:Int8, %1
  %3 = Eq %2, 255:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, %1, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Sub 1:Int8, %1
  %3 = Eq %2, 255:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, 2:Int8, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)");
  });

  suite.test("backwards_or_false").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Bool, flags={}, aliasing=0, offset=1
  %3 = Or %1, %2
  Branch %3, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Store %0, %1, aliasing=0, offset=2
  Store %0, %2, aliasing=0, offset=3
  Exit
}
)", {0, 2}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Bool, flags={}, aliasing=0, offset=1
  %3 = Or %1, %2
  Branch %3, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Store %0, 0:Bool, aliasing=0, offset=2
  Store %0, 0:Bool, aliasing=0, offset=3
  Exit
}
)");
  });

  suite.test("backwards_sub_partial_left").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = And %2, 254:Int8
  %4 = Sub %1, %3
  %5 = ResizeX %4, type=Bool
  Branch %5, true_block=b1, false_block=b2
b1:
  %7 = And %1, 1:Int8
  Store %0, %7, aliasing=0, offset=2
  Exit
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = And %2, 254:Int8
  %4 = Sub %1, %3
  %5 = ResizeX %4, type=Bool
  Branch %5, true_block=b1, false_block=b2
b1:
  %7 = And %1, 1:Int8
  Store %0, 1:Int8, aliasing=0, offset=2
  Exit
b2:
  Exit
}
)");
  });

  suite.test("backwards_sub_partial_right").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = And %2, 254:Int8
  %4 = Sub %3, %1
  %5 = ResizeX %4, type=Bool
  Branch %5, true_block=b1, false_block=b2
b1:
  %7 = And %1, 1:Int8
  Store %0, %7, aliasing=0, offset=2
  Exit
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = And %2, 254:Int8
  %4 = Sub %3, %1
  %5 = ResizeX %4, type=Bool
  Branch %5, true_block=b1, false_block=b2
b1:
  %7 = And %1, 1:Int8
  Store %0, 1:Int8, aliasing=0, offset=2
  Exit
b2:
  Exit
}
)");
  });

  suite.test("backwards_eq_constant_left").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Eq 42:Int8, %1
  Branch %2, true_block=b1, false_block=b2
b1:
  Store %0, %1, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Eq 42:Int8, %1
  Branch %2, true_block=b1, false_block=b2
b1:
  Store %0, 42:Int8, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)");
  });

  suite.test("backwards_eq_partial_intersection").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = And %1, 254:Int8
  %4 = Or %2, 2:Int8
  %5 = Eq %3, %4
  Branch %5, true_block=b1, false_block=b2
b1:
  %7 = And %1, 2:Int8
  %8 = And %2, 1:Int8
  Store %0, %7, aliasing=0, offset=2
  Store %0, %8, aliasing=0, offset=3
  Exit
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = And %1, 254:Int8
  %4 = Or %2, 2:Int8
  %5 = Eq %3, %4
  Branch %5, true_block=b1, false_block=b2
b1:
  %7 = And %1, 2:Int8
  %8 = And %2, 1:Int8
  Store %0, 2:Int8, aliasing=0, offset=2
  Store %0, 0:Int8, aliasing=0, offset=3
  Exit
b2:
  Exit
}
)");
  });

  suite.test("backwards_eq_false_keeps_partial_bits").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = And %1, 254:Int8
  %4 = Or %2, 2:Int8
  %5 = Eq %3, %4
  Branch %5, true_block=b1, false_block=b2
b1:
  Exit
b2:
  %8 = And %1, 2:Int8
  %9 = And %2, 1:Int8
  Store %0, %8, aliasing=0, offset=2
  Store %0, %9, aliasing=0, offset=3
  Exit
}
)";
    check_trace_simplify(ir, {0, 2}, ir);
  });

  suite.test("backwards_eq_intersection_becomes_constant").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = And %1, 15:Int8
  %4 = And %2, 240:Int8
  %5 = Eq %3, %4
  Branch %5, true_block=b1, false_block=b2
b1:
  Store %0, %3, aliasing=0, offset=2
  Store %0, %4, aliasing=0, offset=3
  Exit
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = And %1, 15:Int8
  %4 = And %2, 240:Int8
  %5 = Eq %3, %4
  Branch %5, true_block=b1, false_block=b2
b1:
  Store %0, 0:Int8, aliasing=0, offset=2
  Store %0, 0:Int8, aliasing=0, offset=3
  Exit
b2:
  Exit
}
)");
  });

  suite.test("backwards_shr_u_keeps_discarded_bits_unknown").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = ShrU %1, 3:Int8
  %3 = Eq %2, 22:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %1, 248:Int8
  %6 = And %1, 7:Int8
  Store %0, %5, aliasing=0, offset=1
  Store %0, %6, aliasing=0, offset=2
  Exit
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = ShrU %1, 3:Int8
  %3 = Eq %2, 22:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %1, 248:Int8
  %6 = And %1, 7:Int8
  Store %0, 176:Int8, aliasing=0, offset=1
  Store %0, %6, aliasing=0, offset=2
  Exit
b2:
  Exit
}
)");
  });

  suite.test("backwards_shr_u_partial").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = ShrU %1, 3:Int8
  %3 = ResizeX %2, type=Bool
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %1, 8:Int8
  Store %0, %5, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = ShrU %1, 3:Int8
  %3 = ResizeX %2, type=Bool
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %1, 8:Int8
  Store %0, 8:Int8, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)");
  });

  suite.test("backwards_shr_u_high_bit").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int64, flags={}, aliasing=0, offset=0
  %2 = ShrU %1, 63:Int64
  %3 = ResizeX %2, type=Bool
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %1, 9223372036854775808:Int64
  Store %0, %5, aliasing=0, offset=8
  Exit
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int64, flags={}, aliasing=0, offset=0
  %2 = ShrU %1, 63:Int64
  %3 = LtS %1, 0:Int64
  %4 = ResizeX %2, type=Bool
  Branch %3, true_block=b1, false_block=b2
b1:
  %6 = And %1, 9223372036854775808:Int64
  Store %0, 9223372036854775808:Int64, aliasing=0, offset=8
  Exit
b2:
  Exit
}
)");
  });

  suite.test("backwards_shr_u_zero").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = ShrU %1, 0:Int8
  %3 = Eq %2, 42:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, %1, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = ShrU %1, 0:Int8
  %3 = Eq %1, 42:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, 42:Int8, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)");
  });

  suite.test("backwards_shr_u_variable_shift").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = And %2, 7:Int8
  %4 = ShrU %1, %3
  %5 = Eq %4, 1:Int8
  Branch %5, true_block=b1, false_block=b2
b1:
  Store %0, %1, aliasing=0, offset=2
  Exit
b2:
  Exit
}
)";
    check_trace_simplify(ir, {0, 1}, ir);
  });

  suite.test("substitute_loop_arguments_after_guard").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  Jump %1, %1, block=b1
b1(%3: Int8, %4: Int8):
  %5 = Eq %3, 42:Int8
  Branch %5, true_block=b2, false_block=b3
b2:
  Store %0, %3, aliasing=0, offset=1
  Jump 0:Int8, %3, block=b1
b3:
  Exit
}
)", {1, 2}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  Jump %1, %1, block=b1
b1(%3: Int8, %4: Int8):
  %5 = Eq %3, 42:Int8
  Branch %5, true_block=b2, false_block=b3
b2:
  Store %0, 42:Int8, aliasing=0, offset=1
  Jump 0:Int8, 42:Int8, block=b1
b3:
  Exit
}
)");
  });

  suite.test("substitute_alias_after_guard").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Select 1:Bool, %1, 0:Int8
  %3 = Eq %1, 42:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = Add %2, 1:Int8
  Store %0, %5, aliasing=0, offset=1
  Store %0, %2, aliasing=0, offset=2
  Exit
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Select 1:Bool, %1, 0:Int8
  %3 = Eq %1, 42:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = Add 42:Int8, 1:Int8
  Store %0, 43:Int8, aliasing=0, offset=1
  Store %0, 42:Int8, aliasing=0, offset=2
  Exit
b2:
  Exit
}
)");
  });

  suite.test("backwards_select_known_true").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = Load %0, type=Int8, flags={}, aliasing=0, offset=2
  %4 = Select %1, %2, %3
  %5 = Eq %4, 42:Int8
  Branch %1, true_block=b1, false_block=b2
b1:
  Branch %5, true_block=b3, false_block=b2
b2:
  Exit
b3:
  Store %0, %2, aliasing=0, offset=3
  Store %0, %3, aliasing=0, offset=4
  Exit
}
)", {0, 1, 3}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = Load %0, type=Int8, flags={}, aliasing=0, offset=2
  %4 = Select %1, %2, %3
  %5 = Eq %4, 42:Int8
  Branch %1, true_block=b1, false_block=b2
b1:
  Branch %5, true_block=b3, false_block=b2
b2:
  Exit
b3:
  Store %0, 42:Int8, aliasing=0, offset=3
  Store %0, %3, aliasing=0, offset=4
  Exit
}
)");
  });

  suite.test("backwards_select_known_true_partial").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = Load %0, type=Int8, flags={}, aliasing=0, offset=2
  %4 = Select %1, %2, %3
  %5 = ResizeX %4, type=Bool
  Branch %1, true_block=b1, false_block=b2
b1:
  Branch %5, true_block=b3, false_block=b2
b2:
  Exit
b3:
  %9 = And %2, 1:Int8
  %10 = And %3, 1:Int8
  Store %0, %9, aliasing=0, offset=3
  Store %0, %10, aliasing=0, offset=4
  Exit
}
)", {0, 1, 3}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = Load %0, type=Int8, flags={}, aliasing=0, offset=2
  %4 = Select %1, %2, %3
  %5 = ResizeX %4, type=Bool
  Branch %1, true_block=b1, false_block=b2
b1:
  Branch %5, true_block=b3, false_block=b2
b2:
  Exit
b3:
  %9 = And %2, 1:Int8
  %10 = And %3, 1:Int8
  Store %0, 1:Int8, aliasing=0, offset=3
  Store %0, %10, aliasing=0, offset=4
  Exit
}
)");
  });

  suite.test("backwards_select_known_false").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = Load %0, type=Int8, flags={}, aliasing=0, offset=2
  %4 = Select %1, %2, %3
  %5 = Eq %4, 42:Int8
  Branch %1, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Branch %5, true_block=b3, false_block=b4
b3:
  Store %0, %2, aliasing=0, offset=3
  Store %0, %3, aliasing=0, offset=4
  Exit
b4:
  Exit
}
)", {0, 2, 3}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = Load %0, type=Int8, flags={}, aliasing=0, offset=2
  %4 = Select %1, %2, %3
  %5 = Eq %4, 42:Int8
  Branch %1, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Branch %5, true_block=b3, false_block=b4
b3:
  Store %0, %2, aliasing=0, offset=3
  Store %0, 42:Int8, aliasing=0, offset=4
  Exit
b4:
  Exit
}
)");
  });

  suite.test("backwards_select_known_false_partial").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = Load %0, type=Int8, flags={}, aliasing=0, offset=2
  %4 = Select %1, %2, %3
  %5 = ResizeX %4, type=Bool
  Branch %1, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Branch %5, true_block=b3, false_block=b4
b3:
  %9 = And %2, 1:Int8
  %10 = And %3, 1:Int8
  Store %0, %9, aliasing=0, offset=3
  Store %0, %10, aliasing=0, offset=4
  Exit
b4:
  Exit
}
)", {0, 2, 3}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = Load %0, type=Int8, flags={}, aliasing=0, offset=2
  %4 = Select %1, %2, %3
  %5 = ResizeX %4, type=Bool
  Branch %1, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Branch %5, true_block=b3, false_block=b4
b3:
  %9 = And %2, 1:Int8
  %10 = And %3, 1:Int8
  Store %0, %9, aliasing=0, offset=3
  Store %0, 1:Int8, aliasing=0, offset=4
  Exit
b4:
  Exit
}
)");
  });

  suite.test("backwards_eq_false_bool_0_right").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Eq %1, 0:Bool
  Branch %2, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Store %0, %1, aliasing=0, offset=1
  Exit
}
)", {0, 2}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Xor %1, 1:Bool
  %3 = Eq %1, 0:Bool
  Branch %2, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Store %0, 1:Bool, aliasing=0, offset=1
  Exit
}
)");
  });

  suite.test("backwards_eq_false_bool_0_left").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Eq 0:Bool, %1
  Branch %2, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Store %0, %1, aliasing=0, offset=1
  Exit
}
)", {0, 2}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Xor %1, 1:Bool
  %3 = Eq 0:Bool, %1
  Branch %2, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Store %0, 1:Bool, aliasing=0, offset=1
  Exit
}
)");
  });

  suite.test("backwards_eq_false_bool_1_right").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Eq %1, 1:Bool
  Branch %2, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Store %0, %1, aliasing=0, offset=1
  Exit
}
)", {0, 2}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Eq %1, 1:Bool
  Branch %1, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Store %0, 0:Bool, aliasing=0, offset=1
  Exit
}
)");
  });

  suite.test("backwards_eq_false_bool_1_left").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Eq 1:Bool, %1
  Branch %2, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Store %0, %1, aliasing=0, offset=1
  Exit
}
)", {0, 2}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Eq 1:Bool, %1
  Branch %1, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Store %0, 0:Bool, aliasing=0, offset=1
  Exit
}
)");
  });

  suite.test("backwards_eq_false_bool_guard_known_operand").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Bool, flags={}, aliasing=0, offset=1
  %3 = Eq %1, %2
  Branch %1, true_block=b1, false_block=b2
b1:
  Branch %3, true_block=b2, false_block=b3
b2:
  Exit
b3:
  Store %0, %1, aliasing=0, offset=2
  Store %0, %2, aliasing=0, offset=3
  Exit
}
)", {0, 1, 3}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Bool, flags={}, aliasing=0, offset=1
  %3 = Eq %1, %2
  Branch %1, true_block=b1, false_block=b2
b1:
  Branch %3, true_block=b2, false_block=b3
b2:
  Exit
b3:
  Store %0, 1:Bool, aliasing=0, offset=2
  Store %0, 0:Bool, aliasing=0, offset=3
  Exit
}
)");
  });

  suite.test("backwards_eq_false_bool_unknown_operands").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Bool, flags={}, aliasing=0, offset=1
  %3 = Eq %1, %2
  Branch %3, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Store %0, %1, aliasing=0, offset=2
  Store %0, %2, aliasing=0, offset=3
  Exit
}
)";
    check_trace_simplify(ir, {0, 2}, ir);
  });

  suite.test("backwards_eq_false_integer_zero").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Eq %1, 0:Int8
  Branch %2, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Store %0, %1, aliasing=0, offset=1
  Exit
}
)";
    check_trace_simplify(ir, {0, 2}, ir);
  });

  suite.test("and_reassociate_right_right").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = And %1, 15:Int8
  %3 = And %2, 6:Int8
  Store %0, %2, aliasing=0, offset=1
  Store %0, %3, aliasing=0, offset=2
  Exit
}
)", {0}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = And %1, 15:Int8
  %3 = And %1, 6:Int8
  Store %0, %2, aliasing=0, offset=1
  Store %0, %3, aliasing=0, offset=2
  Exit
}
)");
  });

  suite.test("and_reassociate_right_left").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = And %1, 15:Int8
  %3 = And 6:Int8, %2
  Store %0, %2, aliasing=0, offset=1
  Store %0, %3, aliasing=0, offset=2
  Exit
}
)", {0}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = And %1, 15:Int8
  %3 = And %1, 6:Int8
  Store %0, %2, aliasing=0, offset=1
  Store %0, %3, aliasing=0, offset=2
  Exit
}
)");
  });

  suite.test("and_reassociate_left_right").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = And 15:Int8, %1
  %3 = And %2, 6:Int8
  Store %0, %2, aliasing=0, offset=1
  Store %0, %3, aliasing=0, offset=2
  Exit
}
)", {0}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = And 15:Int8, %1
  %3 = And %1, 6:Int8
  Store %0, %2, aliasing=0, offset=1
  Store %0, %3, aliasing=0, offset=2
  Exit
}
)");
  });

  suite.test("and_reassociate_left_left").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = And 15:Int8, %1
  %3 = And 6:Int8, %2
  Store %0, %2, aliasing=0, offset=1
  Store %0, %3, aliasing=0, offset=2
  Exit
}
)", {0}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = And 15:Int8, %1
  %3 = And %1, 6:Int8
  Store %0, %2, aliasing=0, offset=1
  Store %0, %3, aliasing=0, offset=2
  Exit
}
)");
  });

  suite.test("and_reassociate_deep").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = And %1, 63:Int8
  %3 = And %2, 30:Int8
  %4 = And %3, 21:Int8
  Store %0, %4, aliasing=0, offset=1
  Exit
}
)", {0}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = And %1, 63:Int8
  %3 = And %1, 30:Int8
  %4 = And %1, 20:Int8
  Store %0, %4, aliasing=0, offset=1
  Exit
}
)");
  });

  suite.test("and_reassociate_disjoint_masks").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = And %1, 15:Int8
  %3 = And %2, 240:Int8
  Store %0, %3, aliasing=0, offset=1
  Exit
}
)", {0}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = And %1, 15:Int8
  %3 = And %1, 0:Int8
  Store %0, 0:Int8, aliasing=0, offset=1
  Exit
}
)");
  });

  suite.test("and_reassociate_high_bit").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int64, flags={}, aliasing=0, offset=0
  %2 = And %1, 9223372036854775823:Int64
  %3 = And %2, 9223372036854775814:Int64
  Store %0, %3, aliasing=0, offset=8
  Exit
}
)", {0}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int64, flags={}, aliasing=0, offset=0
  %2 = And %1, 9223372036854775823:Int64
  %3 = And %1, 9223372036854775814:Int64
  Store %0, %3, aliasing=0, offset=8
  Exit
}
)");
  });

  suite.test("and_reassociate_requires_inner_constant").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = And %1, %2
  %4 = And %3, 6:Int8
  Store %0, %4, aliasing=0, offset=2
  Exit
}
)";
    check_trace_simplify(ir, {0}, ir);
  });

  suite.test("contradictory_true_guard_stops_before_shift").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int64, flags={}, aliasing=0, offset=0
  %2 = Eq %1, 64:Int64
  Branch %2, true_block=b1, false_block=b2
b1:
  %4 = Eq %1, 0:Int64
  Branch %4, true_block=b3, false_block=b2
b2:
  Exit
b3:
  %7 = ShrU 1:Int64, %1
  Store %0, %7, aliasing=0, offset=8
  Exit
}
)", {0, 1, 3}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int64, flags={}, aliasing=0, offset=0
  %2 = Eq %1, 64:Int64
  Branch %2, true_block=b1, false_block=b2
b1:
  %4 = Eq 64:Int64, 0:Int64
  Branch 0:Bool, true_block=b3, false_block=b2
b2:
  Exit
b3:
  %7 = ShrU 1:Int64, %1
  Store %0, %7, aliasing=0, offset=8
  Exit
}
)");
  });

  suite.test("contradictory_false_guard_stops_before_division").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int64, flags={}, aliasing=0, offset=0
  %2 = Eq %1, 0:Int64
  Branch %2, true_block=b1, false_block=b2
b1:
  %4 = Eq %1, 0:Int64
  Branch %4, true_block=b2, false_block=b3
b2:
  Exit
b3:
  %7 = DivU 1:Int64, %1
  Store %0, %7, aliasing=0, offset=8
  Exit
}
)", {0, 1, 3}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int64, flags={}, aliasing=0, offset=0
  %2 = Eq %1, 0:Int64
  Branch %2, true_block=b1, false_block=b2
b1:
  %4 = Eq 0:Int64, 0:Int64
  Branch 1:Bool, true_block=b2, false_block=b3
b2:
  Exit
b3:
  %7 = DivU 1:Int64, %1
  Store %0, %7, aliasing=0, offset=8
  Exit
}
)");
  });

  suite.test("contradictory_pending_facts").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Xor %1, 1:Bool
  %3 = And %1, %2
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = Or %1, 0:Bool
  Store %0, %5, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)";
    check_trace_simplify(ir, {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Xor %1, 1:Bool
  %3 = And %1, %2
  Branch 0:Bool, true_block=b1, false_block=b2
b1:
  %5 = Or %1, 0:Bool
  Store %0, %5, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)");
  });

  suite.test("contradiction_while_enqueuing").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Xor %1, 1:Bool
  %3 = And %2, %1
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = Or %1, 0:Bool
  Store %0, %5, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)";
    check_trace_simplify(ir, {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Xor %1, 1:Bool
  %3 = And %2, %1
  Branch 0:Bool, true_block=b1, false_block=b2
b1:
  %5 = Or %1, 0:Bool
  Store %0, %5, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)");
  });

  suite.test("deep_backwards_propagation").run([]() {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* entry = builder.build_block({Type::Ptr, Type::Int64});
    builder.move_to_end(entry);
    Value* value = entry->arg(1);
    const uint64_t depth = 100000;
    for (uint64_t index = 0; index < depth; index++) {
      value = builder.build_add(value, builder.build_const(Type::Int64, 1));
    }
    Value* cond = builder.build_eq(value, builder.build_const(Type::Int64, 42));
    Block* success = builder.build_block();
    Block* failure = builder.build_block();
    builder.build_branch(cond, success, failure);
    builder.move_to_end(success);
    StoreInst* store = builder.build_store(entry->arg(0), entry->arg(1), AliasingGroup(0), 0);
    builder.build_exit();
    builder.move_to_end(failure);
    builder.build_exit();

    Chain chain({entry, success});
    SimplifyTrace::run(&section, &chain);
    Const* result = dynamic_cast<Const*>(store->value());
    unittest_assert(result);
    unittest_assert(result->value() == uint64_t(42) - depth);
  });

  suite.test("select_false_guard_nonconstant_operand").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = Load %0, type=Int8, flags={}, aliasing=0, offset=2
  Branch %1, true_block=b1, false_block=b2
b1:
  Exit
b2:
  %6 = Select %1, %2, %3
  Store %0, %6, aliasing=0, offset=3
  Exit
}
)", {0, 2}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = Load %0, type=Int8, flags={}, aliasing=0, offset=2
  Branch %1, true_block=b1, false_block=b2
b1:
  Exit
b2:
  %6 = Select 0:Bool, %2, %3
  Store %0, %3, aliasing=0, offset=3
  Exit
}
)");
  });

  suite.test("backwards_select_infers_false_condition").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = Select %1, 4:Int8, %2
  %4 = Eq %3, 7:Int8
  Branch %4, true_block=b1, false_block=b2
b1:
  Store %0, %1, aliasing=0, offset=2
  Store %0, %2, aliasing=0, offset=3
  Exit
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = Select %1, 4:Int8, %2
  %4 = Eq %3, 7:Int8
  Branch %4, true_block=b1, false_block=b2
b1:
  Store %0, 0:Bool, aliasing=0, offset=2
  Store %0, 7:Int8, aliasing=0, offset=3
  Exit
b2:
  Exit
}
)");
  });

  suite.test("and_or_partial_knownbits_operand_substitution").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = And %1, 15:Int8
  %4 = Or %2, 15:Int8
  %5 = And %3, %4
  %6 = Or %3, %4
  %7 = Or %4, %3
  Store %0, %5, aliasing=0, offset=2
  Store %0, %6, aliasing=0, offset=3
  Store %0, %7, aliasing=0, offset=4
  Exit
}
)", {0}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = And %1, 15:Int8
  %4 = Or %2, 15:Int8
  %5 = And %3, %4
  %6 = Or %3, %4
  %7 = Or %4, %3
  Store %0, %3, aliasing=0, offset=2
  Store %0, %4, aliasing=0, offset=3
  Store %0, %4, aliasing=0, offset=4
  Exit
}
)");
  });

  suite.test("duplicate_pending_facts").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = And %1, %1
  Branch %2, true_block=b1, false_block=b2
b1:
  Store %0, %1, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = And %1, %1
  Branch %1, true_block=b1, false_block=b2
b1:
  Store %0, 1:Bool, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)");
  });

  suite.test("backwards_eq_contradictory_operand_facts").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = Eq %1, %2
  %4 = Eq %1, 1:Int8
  %5 = Eq %2, 2:Int8
  Branch %4, true_block=b1, false_block=b2
b1:
  Branch %5, true_block=b3, false_block=b4
b2:
  Exit
b3:
  Branch %3, true_block=b5, false_block=b6
b4:
  Exit
b5:
  %11 = And %1, 0:Int8
  Store %0, %11, aliasing=0, offset=2
  Exit
b6:
  Exit
}
)";
    check_trace_simplify(ir, {0, 1, 3, 5}, ir);
  });

  suite.test("and_or_partial_knownbits_insufficient_for_substitution").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = And %1, 31:Int8
  %4 = Or %2, 15:Int8
  %5 = And %3, %4
  %6 = Or %3, %4
  %7 = Or %4, %3
  Store %0, %5, aliasing=0, offset=2
  Store %0, %6, aliasing=0, offset=3
  Store %0, %7, aliasing=0, offset=4
  Exit
}
)";
    check_trace_simplify(ir, {0}, ir);
  });

  return suite.finish();
}
