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

void check_simplify(const std::string& expected, Section* section) {
  metajit::Simplify::run(section, 1);
  std::stringstream ss;
  section->write(ss);
  if (ss.str() != expected) {
    std::cerr << "Expected:\n" << expected << "\n\nGot:\n" << ss.str() << std::endl;
  }
  unittest_assert(ss.str() == expected);
}

void check_simplify_text(const std::string& input, const std::string& expected) {
  Context context;
  Allocator allocator;
  std::istringstream input_stream(input);
  Section* section = SectionReader<Builder>::read_section(context, allocator, input_stream);
  section->order_blocks(BlockOrdering::Dominator);
  unittest_assert(!section->verify(std::cout));
  std::istringstream expected_stream(expected);
  Section* expected_section = SectionReader<Builder>::read_section(context, allocator, expected_stream);
  std::ostringstream normalized;
  expected_section->write(normalized);
  check_simplify(normalized.str(), section);
  unittest_assert(!section->verify(std::cout));
  delete expected_section;
  delete section;
}

void test_simplify_overwritten_bytes() {
  check_simplify_text(R"(section {
b0(%0: Ptr, %1: Int16, %2: Int16, %3: Int16, %4: Int16):
  %5 = Or %1, %2
  %6 = And %5, 65280:Int16
  %7 = Or %6, %3
  %8 = And %7, 255:Int16
  %9 = Or %8, %4
  Store %0, %9, aliasing=0, offset=0
  Exit
}
)", R"(section {
b0(%0: Ptr, %1: Int16, %2: Int16, %3: Int16, %4: Int16):
  %5 = And %3, 255:Int16
  %6 = Or %5, %4
  Store %0, %6, aliasing=0, offset=0
  Exit
}
)");
}

void test_simplify_used_bits_other_use() {
  const std::string input = R"(section {
b0(%0: Ptr, %1: Int16, %2: Int16, %3: Int16, %4: Int16):
  %5 = Or %1, %2
  Store %0, %5, aliasing=0, offset=2
  %7 = And %5, 65280:Int16
  %8 = Or %7, %3
  %9 = And %8, 255:Int16
  %10 = Or %9, %4
  Store %0, %10, aliasing=0, offset=0
  Exit
}
)";
  check_simplify_text(input, R"(section {
b0(%0: Ptr, %1: Int16, %2: Int16, %3: Int16, %4: Int16):
  %5 = Or %1, %2
  Store %0, %5, aliasing=0, offset=2
  %7 = And %3, 255:Int16
  %8 = Or %7, %4
  Store %0, %8, aliasing=0, offset=0
  Exit
}
)");
}

void test_simplify_used_bits_effects() {
  const std::string input = R"(section {
b0(%0: Ptr, %1: Ptr, %2: Bool, %3: Int32):
  %4 = Load %0, type=Int32, flags={}, aliasing=0, offset=0
  %5 = Call %1, %3, type=Int32, call_conv=Default, flags={}
  Store %0, %3, aliasing=0, offset=4
  Branch %2, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Exit
}
)";
  check_simplify_text(input, input);
}

void test_simplify_used_bits_shl_mask() {
  check_simplify_text(R"(section {
b0(%0: Ptr, %1: Int32):
  %2 = And %1, 255:Int32
  %3 = Shl %2, 24:Int32
  Store %0, %3, aliasing=0, offset=0
  Exit
}
)", R"(section {
b0(%0: Ptr, %1: Int32):
  %2 = Shl %1, 24:Int32
  Store %0, %2, aliasing=0, offset=0
  Exit
}
)");
}

void test_simplify_used_bits_load_address() {
  const std::string input = R"(section {
b0(%0: Ptr, %1: Int64):
  %2 = Add %1, 1:Int64
  %3 = AddPtr %0, %2
  %4 = Load %3, type=Int8, flags={}, aliasing=0, offset=0
  Exit
}
)";
  check_simplify_text(input, input);
}

void test_simplify_used_bits_alloca_size() {
  const std::string input = R"(section {
b0(%0: Int64):
  %1 = Add %0, 1:Int64
  %2 = Alloca %1, align=8
  Exit
}
)";
  check_simplify_text(input, input);
}

void test_simplify_overwritten_bytes_differential(Builder& builder, TestData& data) {
  Value* first = data.input(Type::Int16);
  Value* second = data.input(Type::Int16);
  Value* low = data.input(Type::Int16);
  Value* high = data.input(Type::Int16);
  Value* old = builder.build_or(first, second);
  Value* value = builder.build_and(old, builder.build_const(Type::Int16, 0xff00));
  value = builder.build_or(value, low);
  value = builder.build_and(value, builder.build_const(Type::Int16, 0x00ff));
  value = builder.build_or(value, high);
  data.output(value);
  builder.build_exit();
  builder.section()->order_blocks(BlockOrdering::Dominator);
  Section original(builder.section()->context(), builder.section()->allocator());
  Clone::run(builder.section(), &original);
  Simplify::run(builder.section(), 1);
  check_opt_differential(&original, builder.section(), data);
}

void check_simplifycfg(const std::string& expected, Section* section) {
  unittest_assert (!section->verify(std::cout)); // make sure that we start from a valid cfg
  metajit::SimplifyCFG::run(section);
  unittest_assert (!section->verify(std::cout));
  std::stringstream ss;
  section->write(ss);
  if (ss.str() != expected) {
    std::cerr << "Expected:\n" << expected << "\n\nGot:\n" << ss.str() << std::endl;
  }
  unittest_assert(ss.str() == expected);
}

void check_cse(const std::string& expected, Section* section) {
  unittest_assert(!section->verify(std::cout));
  metajit::CommonSubexprElim::run(section);
  unittest_assert(!section->verify(std::cout));
  std::stringstream ss;
  section->write(ss);
  if (ss.str() != expected) {
    std::cerr << "Expected:\n" << expected << "\n\nGot:\n" << ss.str() << std::endl;
  }
  unittest_assert(ss.str() == expected);
}

void check_block_order(const std::string& expected, Section* section, BlockOrdering target_order = BlockOrdering::Natural) {
  section->order_blocks(target_order);
  unittest_assert (!section->verify(std::cout));
  std::stringstream ss;
  section->write(ss);
  if (ss.str() != expected) {
    std::cerr << "Expected:\n" << expected << "\n\nGot:\n" << ss.str() << std::endl;
  }
  unittest_assert(ss.str() == expected);
}

void test_and_or_idempotence_simplify(Builder& builder, TestData& data) {
  Value* input = data.input(Type::Int32);
  Value* value = data.input(Type::Int32);
  Value* other = data.input(Type::Int32);
  Value* masked = builder.fold_and(input, builder.build_const(Type::Int32, 1));
  Value* condition = builder.fold_eq(masked, builder.build_const(Type::Int32, 2));
  Value* selected = builder.fold_select(condition, other, value);
  Value* and_result = builder.fold_and(value, selected);
  Value* or_result = builder.fold_or(value, selected);
  unittest_assert(selected != value); // check that folding couldn't do the optimization
  unittest_assert(and_result != value);
  unittest_assert(or_result != value);
  data.output(and_result);
  StoreInst* and_output = dynamic_cast<StoreInst*>(*builder.block()->rbegin());
  unittest_assert(and_output);
  data.output(or_result);
  StoreInst* or_output = dynamic_cast<StoreInst*>(*builder.block()->rbegin());
  unittest_assert(or_output);
  builder.build_exit();

  Simplify::run(builder.section(), 1);

  unittest_assert(and_output->arg(1) == value);
  unittest_assert(or_output->arg(1) == value);
}

void test_natural_order_deep_chain() {
  Context context;
  Allocator allocator;
  Section section(context, allocator);
  Builder builder(&section);
  std::vector<Block*> blocks;
  const size_t block_count = 100000;
  blocks.reserve(block_count);
  for (size_t index = 0; index < block_count; index++) {
    blocks.push_back(builder.build_block());
  }
  for (size_t index = 0; index + 1 < block_count; index++) {
    builder.move_to_end(blocks[index]);
    builder.build_jump(blocks[index + 1]);
  }
  builder.move_to_end(blocks.back());
  builder.build_exit();
  for (size_t index = block_count; index-- > 1;) {
    section.remove(blocks[index]);
    section.add(blocks[index]);
  }
  section.set_ordering(BlockOrdering::None);

  section.order_blocks(BlockOrdering::Natural);

  size_t index = 0;
  for (Block* block : section) {
    unittest_assert(index < block_count);
    unittest_assert(block == blocks[index]);
    index++;
  }
  unittest_assert(index == block_count);
  unittest_assert(section.ordering() == BlockOrdering::Topological);
}

void trace_builder_shared_exits() {
  for (int effect = 0; effect < 4; effect++) {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    TraceBuilder builder(&section);
    Chain chain;
    builder.set_chain(&chain);
    Block* entry = builder.build_block(std::vector<Type>{Type::Bool, Type::Bool, Type::Ptr});
    builder.move_to_end(entry);
    Value* ptr = entry->arg(2);
    Value* value = builder.build_const(Type::Int32, 42);
    unittest_assert(!builder.build_guard_begin(builder.build_eq(value, value)));
    unittest_assert(builder.block() == entry);
    unittest_assert(!entry->terminator());
    unittest_assert(builder.build_guard_begin(entry->arg(0)));
    Block* failure = builder.block();
    builder.build_store(ptr, value, AliasingGroup(0), 0);
    builder.build_call(builder.build_symbol(Type::Ptr, "reentry"), Type::Void, std::vector<Value*>{ptr});
    builder.build_guard_end();
    if (effect == 1) {
      builder.build_store(ptr, value, AliasingGroup(0), 0);
    } else if (effect == 2) {
      builder.build_call(builder.build_symbol(Type::Ptr, "effect"), Type::Void, std::vector<Value*>{ptr});
    } else if (effect == 3) {
      Value* loaded = builder.build_load(ptr, Type::Int32, LoadFlags::None, AliasingGroup(0), 0);
      unittest_assert(!builder.build_store(ptr, loaded, AliasingGroup(0), 0));
    }
    Block* guard_block = builder.block();
    unittest_assert(!builder.build_guard_begin(builder.build_const(Type::Bool, 1)));
    unittest_assert(builder.block() == guard_block);
    unittest_assert(!guard_block->terminator());
    bool needs_closure = builder.build_guard_begin(entry->arg(1));
    unittest_assert(needs_closure == (effect == 1 || effect == 2));
    BranchInst* guard = dynamic_cast<BranchInst*>(guard_block->terminator());
    unittest_assert(guard);
    unittest_assert((guard->false_block() == failure) == !needs_closure);
    if (needs_closure) {
      builder.build_guard_end();
    }
    builder.build_exit();
    unittest_assert(!section.verify(std::cout));
    unittest_assert(chain.size() == 3);
  }
}

void trace_builder_failure_store_preserves_forwarding() {
  for (AliasingGroup aliasing : {AliasingGroup(0), AliasingGroup(-1)}) {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    TraceBuilder builder(&section);
    Block* entry = builder.build_block(std::vector<Type>{Type::Bool, Type::Ptr});
    builder.move_to_end(entry);
    Value* ptr = entry->arg(1);
    Value* value = builder.build_const(Type::Int32, 42);
    builder.build_store(ptr, value, aliasing, 0);
    unittest_assert(builder.build_guard_begin(entry->arg(0)));
    builder.build_store(ptr, builder.build_const(Type::Int32, 7), aliasing, 0);
    builder.build_guard_end();
    unittest_assert(builder.build_load(ptr, Type::Int32, LoadFlags::None, aliasing, 0) == value);
    builder.build_exit();
    unittest_assert(!section.verify(std::cout));
  }
}

void trace_builder_false_guard() {
  Context context;
  Allocator allocator;
  Section section(context, allocator);
  TraceBuilder builder(&section);
  builder.move_to_end(builder.build_block(std::vector<Type>{Type::Ptr}));
  Value* ptr = builder.entry_arg(0);
  unittest_assert(builder.build_guard_begin(builder.build_const(Type::Bool, 0)));
  builder.build_store(ptr, builder.build_const(Type::Int8, 42), 0, 0);
  builder.build_guard_end();
  builder.build_store(ptr, builder.build_const(Type::Int8, 7), 0, 0);
  builder.build_exit();
  unittest_assert(!section.verify(std::cout));
  uint8_t result = 0;
  Interpreter interpreter(&section, {Interpreter::Bits::constant(&result)});
  unittest_assert(interpreter.run() == Interpreter::Event::Exit);
  unittest_assert(result == 42);
}

void trace_builder_failure_load_bypasses_forwarding() {
  for (AliasingGroup aliasing : {AliasingGroup(0), AliasingGroup(-1)}) {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    TraceBuilder builder(&section);
    Block* entry = builder.build_block(std::vector<Type>{Type::Bool, Type::Ptr});
    builder.move_to_end(entry);
    Value* ptr = entry->arg(1);
    Value* value = builder.build_const(Type::Int32, 42);
    builder.build_store(ptr, value, aliasing, 0);
    unittest_assert(builder.build_guard_begin(entry->arg(0)));
    unittest_assert(dynamic_cast<LoadInst*>(builder.build_load(ptr, Type::Int32, LoadFlags::None, aliasing, 0)));
    unittest_assert(builder.build_store(ptr, value, aliasing, 0));
    AliasingGroup uncached = aliasing < 0 ? AliasingGroup(-2) : AliasingGroup(1);
    Value* failure_load = builder.build_load(ptr, Type::Int32, LoadFlags::None, uncached, 0);
    builder.build_guard_end();
    unittest_assert(builder.build_load(ptr, Type::Int32, LoadFlags::None, aliasing, 0) == value);
    Value* success_load = builder.build_load(ptr, Type::Int32, LoadFlags::None, uncached, 0);
    unittest_assert(dynamic_cast<LoadInst*>(success_load));
    unittest_assert(success_load != failure_load);
    unittest_assert(!builder.build_store(ptr, success_load, uncached, 0));
    builder.build_exit();
    unittest_assert(!section.verify(std::cout));
  }
}

void check_fold_text(const std::string& input, const std::string& expected) {
  Context context;
  Allocator allocator;
  std::istringstream input_stream(input);
  Section* section = SectionReader<TraceBuilder>::read_section(context, allocator, input_stream);
  section->order_blocks(BlockOrdering::Dominator);
  unittest_assert(!section->verify(std::cout));
  DeadCodeElim::run(section);
  std::istringstream expected_stream(expected);
  Section* expected_section = SectionReader<Builder>::read_section(context, allocator, expected_stream);
  std::ostringstream actual_text;
  std::ostringstream expected_text;
  section->write(actual_text);
  expected_section->write(expected_text);
  if (actual_text.str() != expected_text.str()) {
    std::cerr << "Expected:\n" << expected_text.str() << "\nGot:\n" << actual_text.str();
  }
  unittest_assert(actual_text.str() == expected_text.str());
  unittest_assert(!section->verify(std::cout));
  delete expected_section;
  delete section;
}

void check_const(Value* value, Type type, uint64_t expected) {
  unittest_assert(dynamic_cast<Const*>(value));
  unittest_assert(value->type() == type);
  unittest_assert(((Const*) value)->value() == expected);
}

void test_fold_add_complement_to_sub() {
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* block = builder.build_block({type, type});
    builder.move_to_end(block);
    Value* a = block->arg(0);
    Value* b = block->arg(1);
    Value* complemented = builder.fold_not(b);
    Const* one = builder.build_const(type, 1);
    for (Value* sum : {builder.fold_add(a, complemented), builder.fold_add(complemented, a)}) {
      for (Value* result : {builder.fold_add(sum, one), builder.fold_add(one, sum)}) {
        unittest_assert(dynamic_cast<SubInst*>(result));
        SubInst* sub = (SubInst*) result;
        unittest_assert(sub->arg(0) == a);
        unittest_assert(sub->arg(1) == b);
        unittest_assert(sub->type() == type);
      }
      Value* different = builder.fold_add(sum, builder.build_const(type, 2));
      unittest_assert(dynamic_cast<AddInst*>(different));
      unittest_assert(((AddInst*) different)->arg(0) == sum);
    }
    Value* sum = builder.fold_add(a, builder.fold_not(a));
    Value* zero = builder.fold_add(sum, one);
    check_const(zero, type, 0);
    Value* different = builder.fold_add(a, builder.fold_xor(b, builder.build_const(type, 2)));
    Value* unchanged = builder.fold_add(different, one);
    unittest_assert(dynamic_cast<AddInst*>(unchanged));
    unittest_assert(((AddInst*) unchanged)->arg(0) == different);
  }
}

void test_fold_sub_eq_zero() {
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* block = builder.build_block({type, type});
    builder.move_to_end(block);
    Value* a = block->arg(0);
    Value* b = block->arg(1);
    Value* difference = builder.build_sub(a, b);
    Const* zero = builder.build_const(type, 0);
    for (Value* result : {builder.fold_eq(difference, zero), builder.fold_eq(zero, difference)}) {
      unittest_assert(dynamic_cast<EqInst*>(result));
      EqInst* eq = (EqInst*) result;
      unittest_assert(eq->arg(0) == a);
      unittest_assert(eq->arg(1) == b);
      unittest_assert(eq->type() == Type::Bool);
    }
    for (Value* result : {builder.fold_ne(difference, zero), builder.fold_ne(zero, difference)}) {
      XorInst* negated = is_not(result);
      unittest_assert(negated);
      unittest_assert(dynamic_cast<EqInst*>(negated->arg(0)));
      EqInst* eq = (EqInst*) negated->arg(0);
      unittest_assert(eq->arg(0) == a);
      unittest_assert(eq->arg(1) == b);
    }
    Value* same = builder.fold_eq(builder.build_sub(a, a), zero);
    check_const(same, Type::Bool, 1);
    Value* different = builder.fold_eq(difference, builder.build_const(type, 1));
    unittest_assert(dynamic_cast<EqInst*>(different));
    unittest_assert(((EqInst*) different)->arg(0) == difference);
  }
}

void test_fold_mul_power_of_two() {
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* block = builder.build_block({type});
    builder.move_to_end(block);
    Value* input = block->arg(0);
    for (size_t shift = 0; shift < type_width(type); shift++) {
      Const* factor = builder.build_const(type, uint64_t(1) << shift);
      for (bool swapped : {false, true}) {
        Value* result = swapped ? builder.fold_mul(factor, input) : builder.fold_mul(input, factor);
        unittest_assert(result->type() == type);
        if (shift == 0) {
          unittest_assert(result == input);
        } else {
          unittest_assert(dynamic_cast<ShlInst*>(result));
          unittest_assert(((ShlInst*) result)->arg(0) == input);
          Value* amount = ((ShlInst*) result)->arg(1);
          check_const(amount, type, shift);
        }
        for (uint64_t value : {uint64_t(0), uint64_t(1), uint64_t(3), type_mask(type)}) {
          Const* constant = builder.build_const(type, value);
          Value* product = swapped ? builder.fold_mul(factor, constant) : builder.fold_mul(constant, factor);
          check_const(product, type, ((constant->value() << shift) & type_mask(type)));
        }
      }
    }
  }
}

void test_fold_mod_u_power_of_two() {
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* block = builder.build_block({type, type});
    builder.move_to_end(block);
    Value* input = block->arg(0);
    for (size_t bit = 0; bit < type_width(type); bit++) {
      uint64_t divisor = uint64_t(1) << bit;
      Value* constant = builder.build_const(type, divisor);
      Value* result = builder.fold_mod_u(input, constant);
      if (bit == 0) {
        check_const(result, type, 0);
      } else {
        AndInst* mask = dynamic_cast<AndInst*>(result);
        unittest_assert(mask && mask->arg(0) == input);
        check_const(mask->arg(1), type, divisor - 1);
      }
      for (uint64_t value : {uint64_t(0), uint64_t(1), uint64_t(3), type_mask(type)}) {
        Value* folded = builder.fold_mod_u(builder.build_const(type, value), constant);
        check_const(folded, type, value % divisor);
      }
    }
    for (uint64_t divisor : {uint64_t(0), uint64_t(3), uint64_t(5), type_mask(type)}) {
      unittest_assert(dynamic_cast<ModUInst*>(builder.fold_mod_u(input, builder.build_const(type, divisor))));
    }
    unittest_assert(dynamic_cast<ModUInst*>(builder.fold_mod_u(input, block->arg(1))));
    TraceBuilder trace(&section);
    trace.move_to_end(block);
    AndInst* traced = dynamic_cast<AndInst*>(trace.build_mod_u(input, builder.build_const(type, 8)));
    unittest_assert(traced && traced->arg(0) == input);
    check_const(traced->arg(1), type, 7);
    builder.build_exit();
    unittest_assert(!section.verify(std::cout));
  }
}

void test_fold_mul_power_of_two_negative() {
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* block = builder.build_block({type, type});
    builder.move_to_end(block);
    Value* input = block->arg(0);
    for (bool swapped : {false, true}) {
      Const* zero = builder.build_const(type, 0);
      Value* product = swapped ? builder.fold_mul(zero, input) : builder.fold_mul(input, zero);
      check_const(product, type, 0);
      for (uint64_t value : {uint64_t(3), uint64_t(5), type_mask(type)}) {
        Const* factor = builder.build_const(type, value);
        Value* result = swapped ? builder.fold_mul(factor, input) : builder.fold_mul(input, factor);
        unittest_assert(dynamic_cast<MulInst*>(result));
        unittest_assert(((MulInst*) result)->arg(0) == input);
        unittest_assert(((MulInst*) result)->arg(1) == factor);
      }
    }
    unittest_assert(dynamic_cast<MulInst*>(builder.fold_mul(input, block->arg(1))));
  }
}

void test_fold_sub_same_operand() {
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* block = builder.build_block({type});
    builder.move_to_end(block);
    Value* result = builder.fold_sub(block->arg(0), block->arg(0));
    check_const(result, type, 0);
  }
}

void test_fold_xor_same_operand() {
  for (Type type : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* block = builder.build_block({type});
    builder.move_to_end(block);
    Value* result = builder.fold_xor(block->arg(0), block->arg(0));
    check_const(result, type, 0);
  }
}

void test_fold_lt_u_same_operand() {
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* block = builder.build_block({type});
    builder.move_to_end(block);
    Value* result = builder.fold_lt_u(block->arg(0), block->arg(0));
    check_const(result, Type::Bool, 0);
  }
}

void test_fold_lt_s_same_operand() {
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* block = builder.build_block({type});
    builder.move_to_end(block);
    Value* result = builder.fold_lt_s(block->arg(0), block->arg(0));
    check_const(result, Type::Bool, 0);
  }
}

void test_fold_unmasked_shift_pairs() {
  check_fold_text(R"(section {
b0(%0: Ptr, %1: Int8, %2: Int8, %3: Int8, %4: Int8, %5: Int8, %6: Int8, %7: Int8, %8: Int8, %9: Int8, %10: Int8, %11: Int8, %12: Int8, %13: Int8, %14: Int8, %15: Int8, %16: Int8, %17: Int8, %18: Int8, %19: Int8, %20: Int8, %21: Int8, %22: Int8, %23: Int8, %24: Int8, %25: Int8, %26: Int8):
  %27 = ShrU %1, 1:Int8
  %28 = Shl %27, 1:Int8
  Store %0, %28, aliasing=0, offset=0
  %29 = ShrU %2, 1:Int8
  %30 = Shl %29, 1:Int8
  %31 = Or %2, %30
  Store %0, %31, aliasing=0, offset=8
  %32 = ShrU %3, 1:Int8
  %33 = Shl %32, 1:Int8
  %34 = Or %33, %3
  Store %0, %34, aliasing=0, offset=16
  %35 = Shl %4, 1:Int8
  %36 = Shl %35, 1:Int8
  Store %0, %36, aliasing=0, offset=24
  %37 = ShrU %5, 1:Int8
  %38 = Shl %37, 4:Int8
  Store %0, %38, aliasing=0, offset=32
  %39 = Shl %6, 1:Int8
  %40 = Shl %39, 4:Int8
  Store %0, %40, aliasing=0, offset=40
  %41 = ShrU %7, 1:Int8
  %42 = Shl %41, 7:Int8
  Store %0, %42, aliasing=0, offset=48
  %43 = Shl %8, 1:Int8
  %44 = Shl %43, 7:Int8
  Store %0, %44, aliasing=0, offset=56
  %45 = ShrU %9, 4:Int8
  %46 = Shl %45, 1:Int8
  Store %0, %46, aliasing=0, offset=64
  %47 = Shl %10, 4:Int8
  %48 = Shl %47, 1:Int8
  Store %0, %48, aliasing=0, offset=72
  %49 = ShrU %11, 4:Int8
  %50 = Shl %49, 4:Int8
  Store %0, %50, aliasing=0, offset=80
  %51 = ShrU %12, 4:Int8
  %52 = Shl %51, 4:Int8
  %53 = Or %12, %52
  Store %0, %53, aliasing=0, offset=88
  %54 = ShrU %13, 4:Int8
  %55 = Shl %54, 4:Int8
  %56 = Or %55, %13
  Store %0, %56, aliasing=0, offset=96
  %57 = Shl %14, 4:Int8
  %58 = Shl %57, 4:Int8
  Store %0, %58, aliasing=0, offset=104
  %59 = ShrU %15, 4:Int8
  %60 = Shl %59, 7:Int8
  Store %0, %60, aliasing=0, offset=112
  %61 = Shl %16, 4:Int8
  %62 = Shl %61, 7:Int8
  Store %0, %62, aliasing=0, offset=120
  %63 = ShrU %17, 7:Int8
  %64 = Shl %63, 1:Int8
  Store %0, %64, aliasing=0, offset=128
  %65 = Shl %18, 7:Int8
  %66 = Shl %65, 1:Int8
  Store %0, %66, aliasing=0, offset=136
  %67 = ShrU %19, 7:Int8
  %68 = Shl %67, 4:Int8
  Store %0, %68, aliasing=0, offset=144
  %69 = Shl %20, 7:Int8
  %70 = Shl %69, 4:Int8
  Store %0, %70, aliasing=0, offset=152
  %71 = ShrU %21, 7:Int8
  %72 = Shl %71, 7:Int8
  Store %0, %72, aliasing=0, offset=160
  %73 = ShrU %22, 7:Int8
  %74 = Shl %73, 7:Int8
  %75 = Or %22, %74
  Store %0, %75, aliasing=0, offset=168
  %76 = ShrU %23, 7:Int8
  %77 = Shl %76, 7:Int8
  %78 = Or %77, %23
  Store %0, %78, aliasing=0, offset=176
  %79 = Shl %24, 7:Int8
  %80 = Shl %79, 7:Int8
  Store %0, %80, aliasing=0, offset=184
  %81 = ShrU %25, 1:Int8
  %82 = And %81, %26
  %83 = Shl %82, 1:Int8
  Store %0, %83, aliasing=0, offset=192
  Exit
}
)", R"(section {
b0(%0: Ptr, %1: Int8, %2: Int8, %3: Int8, %4: Int8, %5: Int8, %6: Int8, %7: Int8, %8: Int8, %9: Int8, %10: Int8, %11: Int8, %12: Int8, %13: Int8, %14: Int8, %15: Int8, %16: Int8, %17: Int8, %18: Int8, %19: Int8, %20: Int8, %21: Int8, %22: Int8, %23: Int8, %24: Int8, %25: Int8, %26: Int8):
  %27 = And %1, 254:Int8
  Store %0, %27, aliasing=0, offset=0
  Store %0, %2, aliasing=0, offset=8
  Store %0, %3, aliasing=0, offset=16
  %28 = Shl %4, 2:Int8
  Store %0, %28, aliasing=0, offset=24
  %29 = Shl %5, 3:Int8
  %30 = And %29, 240:Int8
  Store %0, %30, aliasing=0, offset=32
  %31 = Shl %6, 5:Int8
  Store %0, %31, aliasing=0, offset=40
  %32 = Shl %7, 6:Int8
  %33 = And %32, 128:Int8
  Store %0, %33, aliasing=0, offset=48
  Store %0, 0:Int8, aliasing=0, offset=56
  %34 = ShrU %9, 3:Int8
  %35 = And %34, 254:Int8
  Store %0, %35, aliasing=0, offset=64
  %36 = Shl %10, 5:Int8
  Store %0, %36, aliasing=0, offset=72
  %37 = And %11, 240:Int8
  Store %0, %37, aliasing=0, offset=80
  Store %0, %12, aliasing=0, offset=88
  Store %0, %13, aliasing=0, offset=96
  Store %0, 0:Int8, aliasing=0, offset=104
  %38 = Shl %15, 3:Int8
  %39 = And %38, 128:Int8
  Store %0, %39, aliasing=0, offset=112
  Store %0, 0:Int8, aliasing=0, offset=120
  %40 = ShrU %17, 6:Int8
  %41 = And %40, 254:Int8
  Store %0, %41, aliasing=0, offset=128
  Store %0, 0:Int8, aliasing=0, offset=136
  %42 = ShrU %19, 3:Int8
  %43 = And %42, 240:Int8
  Store %0, %43, aliasing=0, offset=144
  Store %0, 0:Int8, aliasing=0, offset=152
  %44 = And %21, 128:Int8
  Store %0, %44, aliasing=0, offset=160
  Store %0, %22, aliasing=0, offset=168
  Store %0, %23, aliasing=0, offset=176
  Store %0, 0:Int8, aliasing=0, offset=184
  %45 = ShrU %25, 1:Int8
  %46 = And %45, %26
  %47 = Shl %46, 1:Int8
  Store %0, %47, aliasing=0, offset=192
  Exit
}
)");
}

void test_fold_shift_pairs_int64_boundaries() {
  check_fold_text(R"(section {
b0(%0: Ptr, %1: Int64):
  %2 = ShrU %1, 63:Int64
  %3 = Shl %2, 63:Int64
  Store %0, %3, aliasing=0, offset=0
  %4 = ShrU %1, 63:Int64
  %5 = Shl %4, 1:Int64
  Store %0, %5, aliasing=0, offset=8
  %6 = ShrU %1, 1:Int64
  %7 = Shl %6, 63:Int64
  Store %0, %7, aliasing=0, offset=16
  %8 = Shl %1, 62:Int64
  %9 = Shl %8, 1:Int64
  Store %0, %9, aliasing=0, offset=24
  %10 = Shl %1, 1:Int64
  %11 = Shl %10, 63:Int64
  Store %0, %11, aliasing=0, offset=32
  %12 = Shl %1, 63:Int64
  %13 = Shl %12, 63:Int64
  Store %0, %13, aliasing=0, offset=40
  Exit
}
)", R"(section {
b0(%0: Ptr, %1: Int64):
  %2 = And %1, 9223372036854775808:Int64
  Store %0, %2, aliasing=0, offset=0
  %3 = ShrU %1, 62:Int64
  %4 = And %3, 18446744073709551614:Int64
  Store %0, %4, aliasing=0, offset=8
  %5 = Shl %1, 62:Int64
  %6 = And %5, 9223372036854775808:Int64
  Store %0, %6, aliasing=0, offset=16
  %7 = Shl %1, 63:Int64
  Store %0, %7, aliasing=0, offset=24
  Store %0, 0:Int64, aliasing=0, offset=32
  Store %0, 0:Int64, aliasing=0, offset=40
  Exit
}
)");
}

void test_builder_is_always_equal() {
  Context context;
  Allocator allocator;
  Section section(context, allocator);
  Builder builder(&section);
  Block* block = builder.build_block({Type::Int8, Type::Int8});
  Const* seven = builder.build_const(Type::Int8, 7);
  unittest_assert(builder.is_always_equal(seven, builder.build_const(Type::Int8, 7)));
  unittest_assert(!builder.is_always_equal(seven, builder.build_const(Type::Int8, 8)));
  unittest_assert(!builder.is_always_equal(seven, builder.build_const(Type::Int16, 7)));
  unittest_assert(builder.is_always_equal(block->arg(0), block->arg(0)));
  unittest_assert(!builder.is_always_equal(block->arg(0), block->arg(1)));
  unittest_assert(!builder.is_always_equal(block->arg(0), seven));
  unittest_assert(builder.is_always_equal(nullptr, nullptr));
  unittest_assert(!builder.is_always_equal(nullptr, seven));
  unittest_assert(!builder.is_always_equal(seven, nullptr));
}

void test_fold_equal_constants_factoring() {
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* block = builder.build_block({type, type, Type::Bool});
    builder.move_to_end(block);
    Value* x = block->arg(0);
    Value* y = block->arg(1);
    Const* c1 = builder.build_const(type, 7);
    Const* c2 = builder.build_const(type, 7);
    unittest_assert(c1 != c2);
    for (bool dual : {false, true}) {
      for (bool swap_left : {false, true}) {
        for (bool swap_right : {false, true}) {
          Value* a = swap_left ? c1 : x;
          Value* b = swap_left ? x : c1;
          Value* c = swap_right ? c2 : y;
          Value* d = swap_right ? y : c2;
          Value* left = dual ? (Value*) builder.build_or(a, b) : builder.build_and(a, b);
          Value* right = dual ? (Value*) builder.build_or(c, d) : builder.build_and(c, d);
          for (bool swap_outer : {false, true}) {
            Value* first = swap_outer ? right : left;
            Value* second = swap_outer ? left : right;
            Value* result = dual ? builder.fold_and(first, second) : builder.fold_or(first, second);
            unittest_assert(dual ? dynamic_cast<OrInst*>(result) != nullptr :
                                   dynamic_cast<AndInst*>(result) != nullptr);
            Inst* outer = (Inst*) result;
            check_const(outer->arg(1), type, 7);
            unittest_assert(dual ? dynamic_cast<AndInst*>(outer->arg(0)) != nullptr :
                                   dynamic_cast<OrInst*>(outer->arg(0)) != nullptr);
            Inst* inner = (Inst*) outer->arg(0);
            unittest_assert((inner->arg(0) == x && inner->arg(1) == y) ||
                            (inner->arg(0) == y && inner->arg(1) == x));
          }
        }
      }
    }
  }
}

void test_fold_equal_constants_absorption() {
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* block = builder.build_block({type, type, Type::Bool});
    builder.move_to_end(block);
    Value* x = block->arg(0);
    Value* y = block->arg(1);
    Const* c1 = builder.build_const(type, 7);
    Const* c2 = builder.build_const(type, 7);
    unittest_assert(c1 != c2);
    for (Value* inner : {builder.build_or(x, c1), builder.build_or(c1, x)}) {
      unittest_assert(builder.is_always_equal(builder.fold_and(inner, c2), c2));
      unittest_assert(builder.is_always_equal(builder.fold_and(c2, inner), c2));
    }
    for (Value* inner : {builder.build_and(x, c1), builder.build_and(c1, x)}) {
      unittest_assert(builder.is_always_equal(builder.fold_or(inner, c2), c2));
      unittest_assert(builder.is_always_equal(builder.fold_or(c2, inner), c2));
    }
  }
}

void test_fold_equal_constants_complement_absorption() {
  Context context;
  Allocator allocator;
  Section section(context, allocator);
  Builder builder(&section);
  Block* block = builder.build_block({Type::Int8});
  builder.move_to_end(block);
  Value* x = block->arg(0);
  Const* c1 = builder.build_const(Type::Int8, 7);
  Const* c2 = builder.build_const(Type::Int8, 7);
  unittest_assert(c1 != c2);
  Value* complement = builder.build_xor(c2, builder.build_const(Type::Int8, 255));
  for (Value* inner : {builder.build_or(x, c1), builder.build_or(c1, x)}) {
    for (Value* result : {builder.fold_and(inner, complement), builder.fold_and(complement, inner)}) {
      unittest_assert(dynamic_cast<AndInst*>(result));
      Inst* inst = (Inst*) result;
      unittest_assert((inst->arg(0) == x && inst->arg(1) == complement) ||
                      (inst->arg(0) == complement && inst->arg(1) == x));
    }
  }
  for (Value* inner : {builder.build_and(x, c1), builder.build_and(c1, x)}) {
    for (Value* result : {builder.fold_or(inner, complement), builder.fold_or(complement, inner)}) {
      unittest_assert(dynamic_cast<OrInst*>(result));
      Inst* inst = (Inst*) result;
      unittest_assert((inst->arg(0) == x && inst->arg(1) == complement) ||
                      (inst->arg(0) == complement && inst->arg(1) == x));
    }
  }
  Value* inner = builder.build_and(c1, x);
  unittest_assert(builder.fold_and(inner, c2) == inner);
  unittest_assert(builder.fold_and(c2, inner) == inner);
}

void test_fold_equal_constants_repeated_or() {
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* block = builder.build_block({type, type, Type::Bool});
    builder.move_to_end(block);
    Value* x = block->arg(0);
    Value* y = block->arg(1);
    Const* c1 = builder.build_const(type, 7);
    Const* c2 = builder.build_const(type, 7);
    unittest_assert(c1 != c2);
    for (Value* inner : {builder.build_or(x, c1), builder.build_or(c1, x)}) {
      unittest_assert(builder.fold_or(inner, c2) == inner);
      unittest_assert(builder.fold_or(c2, inner) == inner);
    }
  }
}

void test_fold_equal_constants_xor_or() {
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* block = builder.build_block({type, type, Type::Bool});
    builder.move_to_end(block);
    Value* x = block->arg(0);
    Value* y = block->arg(1);
    Const* c1 = builder.build_const(type, 7);
    Const* c2 = builder.build_const(type, 7);
    unittest_assert(c1 != c2);
    for (Value* inner : {builder.build_xor(x, c1), builder.build_xor(c1, x)}) {
      for (Value* result : {builder.fold_or(inner, c2), builder.fold_or(c2, inner)}) {
        unittest_assert(dynamic_cast<OrInst*>(result));
        OrInst* outer = (OrInst*) result;
        unittest_assert(outer->arg(0) == x);
        check_const(outer->arg(1), type, 7);
      }
    }
  }
}

void test_fold_equal_constants_negated_or() {
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* block = builder.build_block({type, type, Type::Bool});
    builder.move_to_end(block);
    Value* x = block->arg(0);
    Value* y = block->arg(1);
    Const* c1 = builder.build_const(type, 7);
    Const* c2 = builder.build_const(type, 7);
    unittest_assert(c1 != c2);
    for (Value* inner : {builder.build_or(x, c1), builder.build_or(c1, x)}) {
      Value* negated = builder.fold_not(inner);
      for (Value* result : {builder.fold_or(negated, c2), builder.fold_or(c2, negated)}) {
        unittest_assert(dynamic_cast<OrInst*>(result));
        OrInst* outer = (OrInst*) result;
        check_const(outer->arg(1), type, 7);
        unittest_assert(is_not(outer->arg(0)));
        unittest_assert(is_not(outer->arg(0))->arg(0) == x);
      }
    }
  }
}

void test_fold_equal_constants_select() {
  for (Type type : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* block = builder.build_block({type, type, Type::Bool});
    builder.move_to_end(block);
    Value* x = block->arg(0);
    Value* y = block->arg(1);
    Const* c1 = builder.build_const(type, 7);
    Const* c2 = builder.build_const(type, 7);
    unittest_assert(c1 != c2);
    unittest_assert(builder.fold_select(block->arg(2), c1, c2) == c1);
    Value* different = builder.fold_select(block->arg(2), c1, builder.build_const(type, 8));
    if (type == Type::Bool) {
      unittest_assert(different == block->arg(2));
    } else {
      unittest_assert(dynamic_cast<SelectInst*>(different));
    }
  }
}

void test_fold_or_factoring() {
  check_fold_text(R"(section {
b0(%0: Ptr, %1: Int32, %2: Int32, %3: Int32, %4: Int32, %5: Int32, %6: Int32, %7: Int32, %8: Int32, %9: Int32, %10: Int32, %11: Int32, %12: Int32, %13: Int32, %14: Int32, %15: Int32, %16: Int32, %17: Int32, %18: Int32, %19: Int32, %20: Int32, %21: Int32, %22: Int32, %23: Int32, %24: Int32, %25: Int32, %26: Int32, %27: Int32, %28: Int32, %29: Int32):
  %30 = And %1, %2
  %31 = And %1, %3
  %32 = Or %30, %31
  Store %0, %32, aliasing=0, offset=0
  %33 = And %4, %6
  %34 = And %4, %5
  %35 = Or %33, %34
  Store %0, %35, aliasing=0, offset=8
  %36 = And %7, %8
  %37 = And %9, %7
  %38 = Or %36, %37
  Store %0, %38, aliasing=0, offset=16
  %39 = And %12, %10
  %40 = And %10, %11
  %41 = Or %39, %40
  Store %0, %41, aliasing=0, offset=24
  %42 = And %14, %13
  %43 = And %13, %15
  %44 = Or %42, %43
  Store %0, %44, aliasing=0, offset=32
  %45 = And %16, %18
  %46 = And %17, %16
  %47 = Or %45, %46
  Store %0, %47, aliasing=0, offset=40
  %48 = And %20, %19
  %49 = And %21, %19
  %50 = Or %48, %49
  Store %0, %50, aliasing=0, offset=48
  %51 = And %24, %22
  %52 = And %23, %22
  %53 = Or %51, %52
  Store %0, %53, aliasing=0, offset=56
  %54 = And %25, 15:Int32
  %55 = And %25, 240:Int32
  %56 = Or %54, %55
  Store %0, %56, aliasing=0, offset=64
  %57 = And %26, %27
  %58 = And %28, %29
  %59 = Or %57, %58
  Store %0, %59, aliasing=0, offset=72
  Exit
}
)", R"(section {
b0(%0: Ptr, %1: Int32, %2: Int32, %3: Int32, %4: Int32, %5: Int32, %6: Int32, %7: Int32, %8: Int32, %9: Int32, %10: Int32, %11: Int32, %12: Int32, %13: Int32, %14: Int32, %15: Int32, %16: Int32, %17: Int32, %18: Int32, %19: Int32, %20: Int32, %21: Int32, %22: Int32, %23: Int32, %24: Int32, %25: Int32, %26: Int32, %27: Int32, %28: Int32, %29: Int32):
  %30 = Or %2, %3
  %31 = And %1, %30
  Store %0, %31, aliasing=0, offset=0
  %32 = Or %6, %5
  %33 = And %4, %32
  Store %0, %33, aliasing=0, offset=8
  %34 = Or %8, %9
  %35 = And %7, %34
  Store %0, %35, aliasing=0, offset=16
  %36 = Or %11, %12
  %37 = And %10, %36
  Store %0, %37, aliasing=0, offset=24
  %38 = Or %15, %14
  %39 = And %13, %38
  Store %0, %39, aliasing=0, offset=32
  %40 = Or %18, %17
  %41 = And %16, %40
  Store %0, %41, aliasing=0, offset=40
  %42 = Or %20, %21
  %43 = And %19, %42
  Store %0, %43, aliasing=0, offset=48
  %44 = Or %24, %23
  %45 = And %22, %44
  Store %0, %45, aliasing=0, offset=56
  %46 = And %25, 255:Int32
  Store %0, %46, aliasing=0, offset=64
  %47 = And %26, %27
  %48 = And %28, %29
  %49 = Or %47, %48
  Store %0, %49, aliasing=0, offset=72
  Exit
}
)");
}

void test_fold_and_factoring() {
  check_fold_text(R"(section {
b0(%0: Ptr, %1: Int32, %2: Int32, %3: Int32, %4: Int32, %5: Int32, %6: Int32, %7: Int32, %8: Int32, %9: Int32, %10: Int32, %11: Int32, %12: Int32, %13: Int32, %14: Int32, %15: Int32, %16: Int32, %17: Int32, %18: Int32, %19: Int32, %20: Int32, %21: Int32, %22: Int32, %23: Int32, %24: Int32, %25: Int32, %26: Int32, %27: Int32, %28: Int32, %29: Int32):
  %30 = Or %1, %2
  %31 = Or %1, %3
  %32 = And %30, %31
  Store %0, %32, aliasing=0, offset=0
  %33 = Or %4, %6
  %34 = Or %4, %5
  %35 = And %33, %34
  Store %0, %35, aliasing=0, offset=8
  %36 = Or %7, %8
  %37 = Or %9, %7
  %38 = And %36, %37
  Store %0, %38, aliasing=0, offset=16
  %39 = Or %12, %10
  %40 = Or %10, %11
  %41 = And %39, %40
  Store %0, %41, aliasing=0, offset=24
  %42 = Or %14, %13
  %43 = Or %13, %15
  %44 = And %42, %43
  Store %0, %44, aliasing=0, offset=32
  %45 = Or %16, %18
  %46 = Or %17, %16
  %47 = And %45, %46
  Store %0, %47, aliasing=0, offset=40
  %48 = Or %20, %19
  %49 = Or %21, %19
  %50 = And %48, %49
  Store %0, %50, aliasing=0, offset=48
  %51 = Or %24, %22
  %52 = Or %23, %22
  %53 = And %51, %52
  Store %0, %53, aliasing=0, offset=56
  %54 = Or %25, 15:Int32
  %55 = Or %25, 240:Int32
  %56 = And %54, %55
  Store %0, %56, aliasing=0, offset=64
  %57 = Or %26, %27
  %58 = Or %28, %29
  %59 = And %57, %58
  Store %0, %59, aliasing=0, offset=72
  Exit
}
)", R"(section {
b0(%0: Ptr, %1: Int32, %2: Int32, %3: Int32, %4: Int32, %5: Int32, %6: Int32, %7: Int32, %8: Int32, %9: Int32, %10: Int32, %11: Int32, %12: Int32, %13: Int32, %14: Int32, %15: Int32, %16: Int32, %17: Int32, %18: Int32, %19: Int32, %20: Int32, %21: Int32, %22: Int32, %23: Int32, %24: Int32, %25: Int32, %26: Int32, %27: Int32, %28: Int32, %29: Int32):
  %30 = And %2, %3
  %31 = Or %1, %30
  Store %0, %31, aliasing=0, offset=0
  %32 = And %6, %5
  %33 = Or %4, %32
  Store %0, %33, aliasing=0, offset=8
  %34 = And %8, %9
  %35 = Or %7, %34
  Store %0, %35, aliasing=0, offset=16
  %36 = And %11, %12
  %37 = Or %10, %36
  Store %0, %37, aliasing=0, offset=24
  %38 = And %15, %14
  %39 = Or %13, %38
  Store %0, %39, aliasing=0, offset=32
  %40 = And %18, %17
  %41 = Or %16, %40
  Store %0, %41, aliasing=0, offset=40
  %42 = And %20, %21
  %43 = Or %19, %42
  Store %0, %43, aliasing=0, offset=48
  %44 = And %24, %23
  %45 = Or %22, %44
  Store %0, %45, aliasing=0, offset=56
  Store %0, %25, aliasing=0, offset=64
  %46 = Or %26, %27
  %47 = Or %28, %29
  %48 = And %46, %47
  Store %0, %48, aliasing=0, offset=72
  Exit
}
)");
}

void check_fold_mixed_absorption(bool dual) {
  for (Type type : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* block = builder.build_block({type, type, type, type});
    builder.move_to_end(block);
    for (bool constant : {false, true}) {
      Value* x1 = constant ? (Value*) builder.build_const(type, 7) : block->arg(0);
      Value* x2 = constant ? (Value*) builder.build_const(type, 7) : block->arg(0);
      if (constant) {
        unittest_assert(x1 != x2);
      }
      for (Value* z : {block->arg(1), block->arg(2)}) {
        for (bool swap_left : {false, true}) {
          for (bool swap_right : {false, true}) {
            Value* a = swap_left ? block->arg(1) : x1;
            Value* b = swap_left ? x1 : block->arg(1);
            Value* c = swap_right ? z : x2;
            Value* d = swap_right ? x2 : z;
            Value* left = dual ? (Value*) builder.build_or(a, b) : builder.build_and(a, b);
            Value* right = dual ? (Value*) builder.build_and(c, d) : builder.build_or(c, d);
            unittest_assert((dual ? builder.fold_and(left, right) : builder.fold_or(left, right)) == right);
            unittest_assert((dual ? builder.fold_and(right, left) : builder.fold_or(right, left)) == right);
          }
        }
      }
    }
    Value* left = dual ? (Value*) builder.build_or(block->arg(0), block->arg(1)) :
                         builder.build_and(block->arg(0), block->arg(1));
    Value* right = dual ? (Value*) builder.build_and(block->arg(2), block->arg(3)) :
                          builder.build_or(block->arg(2), block->arg(3));
    Value* unrelated = dual ? builder.fold_and(left, right) : builder.fold_or(left, right);
    unittest_assert(dual ? dynamic_cast<AndInst*>(unrelated) != nullptr :
                           dynamic_cast<OrInst*>(unrelated) != nullptr);
    unittest_assert(((Inst*) unrelated)->arg(0) == left);
    unittest_assert(((Inst*) unrelated)->arg(1) == right);
    Value* seven = builder.build_const(type, 7);
    Value* eight = builder.build_const(type, 8);
    left = dual ? (Value*) builder.build_or(block->arg(0), seven) : builder.build_and(block->arg(0), seven);
    right = dual ? (Value*) builder.build_and(block->arg(1), eight) : builder.build_or(block->arg(1), eight);
    unrelated = dual ? builder.fold_and(left, right) : builder.fold_or(left, right);
    unittest_assert(((Inst*) unrelated)->arg(0) == left);
    unittest_assert(((Inst*) unrelated)->arg(1) == right);
  }
}

void test_fold_and_mixed_absorption() {
  check_fold_mixed_absorption(true);
}

void test_fold_or_mixed_absorption() {
  check_fold_mixed_absorption(false);
}

void test_fold_and_xor_constant() {
  for (Type type : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* block = builder.build_block({type, type});
    builder.move_to_end(block);
    Value* input = block->arg(0);
    for (uint64_t value : {uint64_t(0), uint64_t(1), uint64_t(3), uint64_t(0x5555555555555555),
                           uint64_t(1) << (type_width(type) - 1), type_mask(type)}) {
      Const* constant = builder.build_const(type, value);
      uint64_t complement = type_mask(type) ^ constant->value();
      for (bool swap_xor : {false, true}) {
        Value* xored = swap_xor ? builder.build_xor(constant, input) : builder.build_xor(input, constant);
        for (bool swap_and : {false, true}) {
          Value* result = swap_and ? builder.fold_and(input, xored) : builder.fold_and(xored, input);
          unittest_assert(result->type() == type);
          if (complement == 0) {
            check_const(result, type, 0);
          } else if (complement == type_mask(type)) {
            unittest_assert(result == input);
          } else {
            unittest_assert(dynamic_cast<AndInst*>(result));
            unittest_assert(((AndInst*) result)->arg(0) == input);
            Value* mask = ((AndInst*) result)->arg(1);
            check_const(mask, type, complement);
          }
        }
      }
    }
    Value* xored = builder.build_xor(input, builder.build_const(type, 1));
    Value* different = builder.fold_and(xored, block->arg(1));
    unittest_assert(dynamic_cast<AndInst*>(different));
    unittest_assert(((AndInst*) different)->arg(0) == xored);
    Value* variable_xor = builder.build_xor(input, block->arg(1));
    Value* variable_result = builder.fold_and(variable_xor, input);
    unittest_assert(dynamic_cast<AndInst*>(variable_result));
    unittest_assert(((AndInst*) variable_result)->arg(0) == variable_xor);
  }
}

void test_fold_and_xor_subset() {
  for (Type type : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* block = builder.build_block({type, type, type});
    builder.move_to_end(block);
    Value* x = block->arg(0);
    for (Value* y : {(Value*) block->arg(1), (Value*) builder.build_const(type, 3)}) {
      for (bool swap_inner : {false, true}) {
        Value* subset = swap_inner ? builder.build_and(y, x) : builder.build_and(x, y);
        for (bool swap_xor : {false, true}) {
          Value* xored = swap_xor ? builder.build_xor(x, subset) : builder.build_xor(subset, x);
          for (bool swap_outer : {false, true}) {
            Value* result = swap_outer ? builder.fold_and(subset, xored) : builder.fold_and(xored, subset);
            check_const(result, type, 0);
          }
        }
      }
    }
    Const* constant_x = builder.build_const(type, 3);
    Const* equal_x = builder.build_const(type, 3);
    unittest_assert(constant_x != equal_x);
    for (bool swap_inner : {false, true}) {
      Value* constant_subset = swap_inner ? builder.build_and(block->arg(1), constant_x) :
                                           builder.build_and(constant_x, block->arg(1));
      Value* constant_xor = builder.build_xor(constant_subset, equal_x);
      Value* constant_result = builder.fold_and(constant_xor, constant_subset);
      check_const(constant_result, type, 0);
    }
    Value* subset = builder.build_and(x, block->arg(1));
    Value* unrelated = builder.build_xor(subset, block->arg(2));
    Value* different_xor = builder.fold_and(unrelated, subset);
    unittest_assert(dynamic_cast<AndInst*>(different_xor));
    unittest_assert(((AndInst*) different_xor)->arg(0) == unrelated);
    Value* xored = builder.build_xor(subset, x);
    Value* different_outer = builder.fold_and(xored, block->arg(2));
    unittest_assert(dynamic_cast<AndInst*>(different_outer));
    unittest_assert(((AndInst*) different_outer)->arg(0) == xored);
  }
}

void test_fold_and_absorption() {
  for (Type type : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* block = builder.build_block({type, type});
    builder.move_to_end(block);
    Value* x = block->arg(0);
    Value* y = block->arg(1);
    for (Value* inner : {builder.build_or(x, y), builder.build_or(y, x)}) {
      unittest_assert(builder.fold_and(x, inner) == x);
      unittest_assert(builder.fold_and(inner, x) == x);
    }
    Value* unrelated = builder.build_or(y, builder.build_const(type, 1));
    unittest_assert(dynamic_cast<AndInst*>(builder.fold_and(x, unrelated)));
  }
}

void test_fold_and_complement_absorption() {
  check_fold_text(R"(section {
b0(%0: Ptr, %1: Int32, %2: Int32, %3: Int32, %4: Int32, %5: Int32, %6: Int32, %7: Int32, %8: Int32, %9: Int32, %10: Int32, %11: Int32, %12: Int32, %13: Int32, %14: Int32, %15: Int32, %16: Int32, %17: Int32, %18: Int32, %19: Int32, %20: Int32, %21: Int32, %22: Int32, %23: Int32, %24: Int32, %25: Int32, %26: Int32):
  %27 = Or %1, %2
  %28 = Xor %1, 4294967295:Int32
  %29 = And %27, %28
  Store %0, %29, aliasing=0, offset=0
  %30 = Xor %3, 4294967295:Int32
  %31 = Or %3, %4
  %32 = And %30, %31
  Store %0, %32, aliasing=0, offset=8
  %33 = Or %5, %6
  %34 = Xor %6, 4294967295:Int32
  %35 = And %33, %34
  Store %0, %35, aliasing=0, offset=16
  %36 = Xor %8, 4294967295:Int32
  %37 = Or %7, %8
  %38 = And %36, %37
  Store %0, %38, aliasing=0, offset=24
  %39 = Or %10, %9
  %40 = Xor %9, 4294967295:Int32
  %41 = And %39, %40
  Store %0, %41, aliasing=0, offset=32
  %42 = Xor %11, 4294967295:Int32
  %43 = Or %12, %11
  %44 = And %42, %43
  Store %0, %44, aliasing=0, offset=40
  %45 = Or %14, %13
  %46 = Xor %14, 4294967295:Int32
  %47 = And %45, %46
  Store %0, %47, aliasing=0, offset=48
  %48 = Xor %16, 4294967295:Int32
  %49 = Or %16, %15
  %50 = And %48, %49
  Store %0, %50, aliasing=0, offset=56
  %51 = Or %17, %18
  %52 = Xor %19, 4294967295:Int32
  %53 = And %51, %52
  Store %0, %53, aliasing=0, offset=64
  %54 = Or %20, %21
  %55 = Xor %21, 1:Int32
  %56 = And %54, %55
  Store %0, %56, aliasing=0, offset=72
  %57 = Or %23, %22
  %58 = Xor %24, 4294967295:Int32
  %59 = And %57, %58
  Store %0, %59, aliasing=0, offset=80
  %60 = Or %26, %25
  %61 = Xor %26, 1:Int32
  %62 = And %60, %61
  Store %0, %62, aliasing=0, offset=88
  Exit
}
)", R"(section {
b0(%0: Ptr, %1: Int32, %2: Int32, %3: Int32, %4: Int32, %5: Int32, %6: Int32, %7: Int32, %8: Int32, %9: Int32, %10: Int32, %11: Int32, %12: Int32, %13: Int32, %14: Int32, %15: Int32, %16: Int32, %17: Int32, %18: Int32, %19: Int32, %20: Int32, %21: Int32, %22: Int32, %23: Int32, %24: Int32, %25: Int32, %26: Int32):
  %27 = Xor %1, 4294967295:Int32
  %28 = And %27, %2
  Store %0, %28, aliasing=0, offset=0
  %29 = Xor %3, 4294967295:Int32
  %30 = And %29, %4
  Store %0, %30, aliasing=0, offset=8
  %31 = Xor %6, 4294967295:Int32
  %32 = And %31, %5
  Store %0, %32, aliasing=0, offset=16
  %33 = Xor %8, 4294967295:Int32
  %34 = And %33, %7
  Store %0, %34, aliasing=0, offset=24
  %35 = Xor %9, 4294967295:Int32
  %36 = And %35, %10
  Store %0, %36, aliasing=0, offset=32
  %37 = Xor %11, 4294967295:Int32
  %38 = And %37, %12
  Store %0, %38, aliasing=0, offset=40
  %39 = Xor %14, 4294967295:Int32
  %40 = And %39, %13
  Store %0, %40, aliasing=0, offset=48
  %41 = Xor %16, 4294967295:Int32
  %42 = And %41, %15
  Store %0, %42, aliasing=0, offset=56
  %43 = Or %17, %18
  %44 = Xor %19, 4294967295:Int32
  %45 = And %43, %44
  Store %0, %45, aliasing=0, offset=64
  %46 = Or %20, %21
  %47 = Xor %21, 1:Int32
  %48 = And %46, %47
  Store %0, %48, aliasing=0, offset=72
  %49 = Or %23, %22
  %50 = Xor %24, 4294967295:Int32
  %51 = And %49, %50
  Store %0, %51, aliasing=0, offset=80
  %52 = Or %26, %25
  %53 = Xor %26, 1:Int32
  %54 = And %52, %53
  Store %0, %54, aliasing=0, offset=88
  Exit
}
)");
}

void test_fold_or_complement_absorption() {
  check_fold_text(R"(section {
b0(%0: Ptr, %1: Int32, %2: Int32, %3: Int32, %4: Int32, %5: Int32, %6: Int32, %7: Int32, %8: Int32, %9: Int32, %10: Int32, %11: Int32, %12: Int32, %13: Int32, %14: Int32, %15: Int32, %16: Int32, %17: Int32, %18: Int32, %19: Int32, %20: Int32, %21: Int32, %22: Int32, %23: Int32, %24: Int32, %25: Int32, %26: Int32):
  %27 = And %1, %2
  %28 = Xor %1, 4294967295:Int32
  %29 = Or %27, %28
  Store %0, %29, aliasing=0, offset=0
  %30 = Xor %3, 4294967295:Int32
  %31 = And %3, %4
  %32 = Or %30, %31
  Store %0, %32, aliasing=0, offset=8
  %33 = And %5, %6
  %34 = Xor %6, 4294967295:Int32
  %35 = Or %33, %34
  Store %0, %35, aliasing=0, offset=16
  %36 = Xor %8, 4294967295:Int32
  %37 = And %7, %8
  %38 = Or %36, %37
  Store %0, %38, aliasing=0, offset=24
  %39 = And %10, %9
  %40 = Xor %9, 4294967295:Int32
  %41 = Or %39, %40
  Store %0, %41, aliasing=0, offset=32
  %42 = Xor %11, 4294967295:Int32
  %43 = And %12, %11
  %44 = Or %42, %43
  Store %0, %44, aliasing=0, offset=40
  %45 = And %14, %13
  %46 = Xor %14, 4294967295:Int32
  %47 = Or %45, %46
  Store %0, %47, aliasing=0, offset=48
  %48 = Xor %16, 4294967295:Int32
  %49 = And %16, %15
  %50 = Or %48, %49
  Store %0, %50, aliasing=0, offset=56
  %51 = And %17, %18
  %52 = Xor %19, 4294967295:Int32
  %53 = Or %51, %52
  Store %0, %53, aliasing=0, offset=64
  %54 = And %20, %21
  %55 = Xor %21, 1:Int32
  %56 = Or %54, %55
  Store %0, %56, aliasing=0, offset=72
  %57 = And %23, %22
  %58 = Xor %24, 4294967295:Int32
  %59 = Or %57, %58
  Store %0, %59, aliasing=0, offset=80
  %60 = And %26, %25
  %61 = Xor %26, 1:Int32
  %62 = Or %60, %61
  Store %0, %62, aliasing=0, offset=88
  Exit
}
)", R"(section {
b0(%0: Ptr, %1: Int32, %2: Int32, %3: Int32, %4: Int32, %5: Int32, %6: Int32, %7: Int32, %8: Int32, %9: Int32, %10: Int32, %11: Int32, %12: Int32, %13: Int32, %14: Int32, %15: Int32, %16: Int32, %17: Int32, %18: Int32, %19: Int32, %20: Int32, %21: Int32, %22: Int32, %23: Int32, %24: Int32, %25: Int32, %26: Int32):
  %27 = Xor %1, 4294967295:Int32
  %28 = Or %2, %27
  Store %0, %28, aliasing=0, offset=0
  %29 = Xor %3, 4294967295:Int32
  %30 = Or %4, %29
  Store %0, %30, aliasing=0, offset=8
  %31 = Xor %6, 4294967295:Int32
  %32 = Or %5, %31
  Store %0, %32, aliasing=0, offset=16
  %33 = Xor %8, 4294967295:Int32
  %34 = Or %7, %33
  Store %0, %34, aliasing=0, offset=24
  %35 = Xor %9, 4294967295:Int32
  %36 = Or %10, %35
  Store %0, %36, aliasing=0, offset=32
  %37 = Xor %11, 4294967295:Int32
  %38 = Or %12, %37
  Store %0, %38, aliasing=0, offset=40
  %39 = Xor %14, 4294967295:Int32
  %40 = Or %13, %39
  Store %0, %40, aliasing=0, offset=48
  %41 = Xor %16, 4294967295:Int32
  %42 = Or %15, %41
  Store %0, %42, aliasing=0, offset=56
  %43 = And %17, %18
  %44 = Xor %19, 4294967295:Int32
  %45 = Or %43, %44
  Store %0, %45, aliasing=0, offset=64
  %46 = And %20, %21
  %47 = Xor %21, 1:Int32
  %48 = Or %46, %47
  Store %0, %48, aliasing=0, offset=72
  %49 = And %23, %22
  %50 = Xor %24, 4294967295:Int32
  %51 = Or %49, %50
  Store %0, %51, aliasing=0, offset=80
  %52 = And %26, %25
  %53 = Xor %26, 1:Int32
  %54 = Or %52, %53
  Store %0, %54, aliasing=0, offset=88
  Exit
}
)");
}

void test_fold_or_absorption() {
  for (Type type : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* block = builder.build_block({type, type});
    builder.move_to_end(block);
    Value* x = block->arg(0);
    Value* y = block->arg(1);
    for (Value* inner : {builder.build_and(x, y), builder.build_and(y, x)}) {
      unittest_assert(builder.fold_or(x, inner) == x);
      unittest_assert(builder.fold_or(inner, x) == x);
    }
    Value* unrelated = builder.build_and(y, builder.build_const(type, 1));
    unittest_assert(dynamic_cast<OrInst*>(builder.fold_or(x, unrelated)));
  }
}

void test_fold_or_negated_or_operand() {
  check_fold_text(R"(section {
b0(%0: Ptr, %1: Int32, %2: Int32, %3: Int32, %4: Int32, %5: Int32, %6: Int32, %7: Int32, %8: Int32, %9: Int32, %10: Int32, %11: Int32, %12: Int32, %13: Int32, %14: Int32, %15: Int32, %16: Int32, %17: Int32, %18: Int32, %19: Int32, %20: Int32, %21: Int32, %22: Int32, %23: Int32, %24: Int32, %25: Int32, %26: Int32, %27: Int32):
  %28 = Or %1, %2
  %29 = Xor %28, 4294967295:Int32
  %30 = Or %1, %29
  Store %0, %30, aliasing=0, offset=0
  %31 = Or %3, %4
  %32 = Xor %31, 4294967295:Int32
  %33 = Or %32, %3
  Store %0, %33, aliasing=0, offset=8
  %34 = Or %5, %6
  %35 = Xor %34, 4294967295:Int32
  %36 = Or %6, %35
  Store %0, %36, aliasing=0, offset=16
  %37 = Or %7, %8
  %38 = Xor %37, 4294967295:Int32
  %39 = Or %38, %8
  Store %0, %39, aliasing=0, offset=24
  %40 = Or %10, %9
  %41 = Xor %40, 4294967295:Int32
  %42 = Or %9, %41
  Store %0, %42, aliasing=0, offset=32
  %43 = Or %12, %11
  %44 = Xor %43, 4294967295:Int32
  %45 = Or %44, %11
  Store %0, %45, aliasing=0, offset=40
  %46 = Or %14, %13
  %47 = Xor %46, 4294967295:Int32
  %48 = Or %14, %47
  Store %0, %48, aliasing=0, offset=48
  %49 = Or %16, %15
  %50 = Xor %49, 4294967295:Int32
  %51 = Or %50, %16
  Store %0, %51, aliasing=0, offset=56
  %52 = Or %17, %18
  %53 = Xor %52, 4294967295:Int32
  %54 = Or %53, %19
  Store %0, %54, aliasing=0, offset=64
  %55 = Or %20, %21
  %56 = Xor %55, 1:Int32
  %57 = Or %20, %56
  Store %0, %57, aliasing=0, offset=72
  %58 = Or %23, %22
  %59 = Xor %58, 4294967295:Int32
  %60 = Or %59, %24
  Store %0, %60, aliasing=0, offset=80
  %61 = Or %26, %25
  %62 = Xor %61, 1:Int32
  %63 = Or %25, %62
  Store %0, %63, aliasing=0, offset=88
  %64 = Or %27, %27
  %65 = Xor %64, 4294967295:Int32
  %66 = Or %27, %65
  Store %0, %66, aliasing=0, offset=96
  Exit
}
)", R"(section {
b0(%0: Ptr, %1: Int32, %2: Int32, %3: Int32, %4: Int32, %5: Int32, %6: Int32, %7: Int32, %8: Int32, %9: Int32, %10: Int32, %11: Int32, %12: Int32, %13: Int32, %14: Int32, %15: Int32, %16: Int32, %17: Int32, %18: Int32, %19: Int32, %20: Int32, %21: Int32, %22: Int32, %23: Int32, %24: Int32, %25: Int32, %26: Int32, %27: Int32):
  %28 = Xor %2, 4294967295:Int32
  %29 = Or %1, %28
  Store %0, %29, aliasing=0, offset=0
  %30 = Xor %4, 4294967295:Int32
  %31 = Or %3, %30
  Store %0, %31, aliasing=0, offset=8
  %32 = Xor %5, 4294967295:Int32
  %33 = Or %6, %32
  Store %0, %33, aliasing=0, offset=16
  %34 = Xor %7, 4294967295:Int32
  %35 = Or %8, %34
  Store %0, %35, aliasing=0, offset=24
  %36 = Xor %10, 4294967295:Int32
  %37 = Or %9, %36
  Store %0, %37, aliasing=0, offset=32
  %38 = Xor %12, 4294967295:Int32
  %39 = Or %11, %38
  Store %0, %39, aliasing=0, offset=40
  %40 = Xor %13, 4294967295:Int32
  %41 = Or %14, %40
  Store %0, %41, aliasing=0, offset=48
  %42 = Xor %15, 4294967295:Int32
  %43 = Or %16, %42
  Store %0, %43, aliasing=0, offset=56
  %44 = Or %17, %18
  %45 = Xor %44, 4294967295:Int32
  %46 = Or %45, %19
  Store %0, %46, aliasing=0, offset=64
  %47 = Or %20, %21
  %48 = Xor %47, 1:Int32
  %49 = Or %20, %48
  Store %0, %49, aliasing=0, offset=72
  %50 = Or %23, %22
  %51 = Xor %50, 4294967295:Int32
  %52 = Or %51, %24
  Store %0, %52, aliasing=0, offset=80
  %53 = Or %26, %25
  %54 = Xor %53, 1:Int32
  %55 = Or %25, %54
  Store %0, %55, aliasing=0, offset=88
  Store %0, 4294967295:Int32, aliasing=0, offset=96
  Exit
}
)");
}

void test_fold_or_xor_operand() {
  for (Type type : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* block = builder.build_block({type, type, type});
    builder.move_to_end(block);
    Value* x = block->arg(0);
    Value* parameter = block->arg(1);
    Value* constant = builder.build_const(type, 1);
    for (Value* y : {parameter, constant}) {
      for (Value* inner : {builder.build_xor(x, y), builder.build_xor(y, x)}) {
        for (Value* repeated : {x, y}) {
          for (Value* result : {builder.fold_or(inner, repeated), builder.fold_or(repeated, inner)}) {
            if (type == Type::Bool && dynamic_cast<Const*>(y)) {
              unittest_assert(result == y);
            } else {
              unittest_assert(dynamic_cast<OrInst*>(result));
              OrInst* or_inst = (OrInst*) result;
              unittest_assert((or_inst->arg(0) == x && or_inst->arg(1) == y) ||
                              (or_inst->arg(0) == y && or_inst->arg(1) == x));
            }
          }
        }
        Value* unrelated = builder.fold_or(inner, block->arg(2));
        unittest_assert(dynamic_cast<OrInst*>(unrelated));
        OrInst* or_inst = (OrInst*) unrelated;
        unittest_assert(or_inst->arg(0) == inner && or_inst->arg(1) == block->arg(2));
      }
    }
  }
}

void test_fold_or_repeated_operand() {
  for (Type type : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* block = builder.build_block({type, type, type});
    builder.move_to_end(block);
    Value* x = block->arg(0);
    Value* y = block->arg(1);
    for (Value* inner : {builder.build_or(x, y), builder.build_or(y, x)}) {
      for (Value* repeated : {x, y}) {
        unittest_assert(builder.fold_or(repeated, inner) == inner);
        unittest_assert(builder.fold_or(inner, repeated) == inner);
      }
      Value* unrelated = builder.fold_or(block->arg(2), inner);
      unittest_assert(dynamic_cast<OrInst*>(unrelated));
      unittest_assert(unrelated != inner);
    }
  }
}

void test_fold_and_repeated_operand() {
  for (Type type : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* block = builder.build_block({type, type, type});
    builder.move_to_end(block);
    Value* x = block->arg(0);
    Value* y = block->arg(1);
    for (Value* inner : {builder.build_and(x, y), builder.build_and(y, x)}) {
      for (Value* repeated : {x, y}) {
        unittest_assert(builder.fold_and(repeated, inner) == inner);
        unittest_assert(builder.fold_and(inner, repeated) == inner);
      }
      Value* unrelated = builder.fold_and(block->arg(2), inner);
      unittest_assert(dynamic_cast<AndInst*>(unrelated));
      unittest_assert(unrelated != inner);
    }
  }
}

void check_fold_nested_truncations(bool inner_unsigned, bool outer_unsigned) {
  for (Type source : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    for (Type intermediate : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
      for (Type target : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
        if (type_width(source) <= type_width(intermediate) || type_width(intermediate) <= type_width(target)) {
          continue;
        }
        Context context;
        Allocator allocator;
        Section section(context, allocator);
        Builder builder(&section);
        Block* block = builder.build_block({source});
        builder.move_to_end(block);
        for (Value* input : {(Value*) block->arg(0), (Value*) builder.build_poison(source)}) {
          Value* inner = inner_unsigned ? (Value*) builder.build_resize_u(input, intermediate) :
                                                  builder.build_resize_x(input, intermediate);
          Value* result = outer_unsigned ? builder.fold_resize_u(inner, target) : builder.fold_resize_x(inner, target);
          unittest_assert(outer_unsigned ? dynamic_cast<ResizeUInst*>(result) != nullptr :
                                           dynamic_cast<ResizeXInst*>(result) != nullptr);
          unittest_assert(result->type() == target);
          unittest_assert(((Inst*) result)->arg(0) == input);
        }
      }
    }
  }
  Context context;
  Allocator allocator;
  Section section(context, allocator);
  Builder builder(&section);
  Block* block = builder.build_block({Type::Int32});
  builder.move_to_end(block);
  Value* inner = inner_unsigned ? (Value*) builder.build_resize_u(block->arg(0), Type::Int8) :
                                          builder.build_resize_x(block->arg(0), Type::Int8);
  Value* result = outer_unsigned ? builder.fold_resize_u(inner, Type::Int16) : builder.fold_resize_x(inner, Type::Int16);
  unittest_assert(((Inst*) result)->arg(0) == inner);
}

void test_fold_narrowing_resize_uu() {
  check_fold_nested_truncations(true, true);
}

void test_fold_narrowing_resize_ux() {
  check_fold_nested_truncations(true, false);
}

void test_fold_narrowing_resize_xu() {
  check_fold_nested_truncations(false, true);
}

void test_fold_nested_unsigned_resizes() {
  for (Type source : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    for (Type intermediate : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
      for (Type target : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
        if (source == intermediate || intermediate == target) {
          continue;
        }
        Context context;
        Allocator allocator;
        Section section(context, allocator);
        Builder builder(&section);
        Block* block = builder.build_block({source});
        builder.move_to_end(block);
        for (Value* input : {(Value*) block->arg(0), (Value*) builder.build_poison(source)}) {
          Value* inner = builder.build_resize_u(input, intermediate);
          Value* result = builder.fold_resize_u(inner, target);
          unittest_assert(dynamic_cast<ResizeUInst*>(result));
          unittest_assert(result->type() == target);
          bool widening = type_width(source) < type_width(intermediate) && type_width(intermediate) < type_width(target);
          bool narrowing = type_width(source) > type_width(intermediate) && type_width(intermediate) > type_width(target);
          Value* expected = widening || narrowing ? input : inner;
          unittest_assert(((ResizeUInst*) result)->arg(0) == expected);
        }
      }
    }
  }
}

void check_fold_extension_roundtrip(bool inner_unsigned) {
  for (Type source : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    for (Type intermediate : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
      if (source == intermediate) {
        continue;
      }
      Context context;
      Allocator allocator;
      Section section(context, allocator);
      Builder builder(&section);
      Block* block = builder.build_block({source});
      builder.move_to_end(block);
      for (Value* input : { (Value*) block->arg(0), (Value*) builder.build_poison(source) }) {
        Value* inner = inner_unsigned ? (Value*) builder.build_resize_u(input, intermediate) :
                                        builder.build_resize_x(input, intermediate);
        Value* result = builder.fold_resize_x(inner, source);
        unittest_assert(result->type() == source);
        if (type_width(source) < type_width(intermediate)) {
          unittest_assert(result == input);
        } else {
          unittest_assert(dynamic_cast<ResizeXInst*>(result));
          unittest_assert(((ResizeXInst*) result)->arg(0) == inner);
        }
      }
    }
  }
  Context context;
  Allocator allocator;
  Section section(context, allocator);
  Builder builder(&section);
  Block* block = builder.build_block({Type::Bool});
  builder.move_to_end(block);
  Value* inner = inner_unsigned ? (Value*) builder.build_resize_u(block->arg(0), Type::Int64) :
                                  builder.build_resize_x(block->arg(0), Type::Int64);
  Value* different_type = builder.fold_resize_x(inner, Type::Int32);
  unittest_assert(dynamic_cast<ResizeXInst*>(different_type));
  unittest_assert(((ResizeXInst*) different_type)->arg(0) == inner);
}

void test_fold_unsigned_extension_roundtrip() {
  check_fold_extension_roundtrip(true);
}

void test_fold_unspecified_extension_roundtrip() {
  check_fold_extension_roundtrip(false);
}

void test_fold_nested_narrowing_resize_x() {
  for (Type source : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    for (Type intermediate : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
      for (Type target : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
        if (source == intermediate || intermediate == target) {
          continue;
        }
        Context context;
        Allocator allocator;
        Section section(context, allocator);
        Builder builder(&section);
        Block* block = builder.build_block({source});
        builder.move_to_end(block);
        Value* input = block->arg(0);
        Value* inner = builder.build_resize_x(input, intermediate);
        Value* result = builder.fold_resize_x(inner, target);
        unittest_assert(result->type() == target);
        if (source == target && type_width(source) < type_width(intermediate)) {
          unittest_assert(result == input);
          continue;
        }
        unittest_assert(dynamic_cast<ResizeXInst*>(result));
        Value* expected = type_width(target) < type_width(intermediate) &&
                          type_width(intermediate) < type_width(source) ? input : inner;
        unittest_assert(((ResizeXInst*) result)->arg(0) == expected);
      }
    }
  }
}

void test_fold_resize_x_sign_bit() {
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* block = builder.build_block({type, type});
    builder.move_to_end(block);
    Value* input = block->arg(0);
    Value* shifted = builder.build_shr_u(input, builder.build_const(type, type_width(type) - 1));
    Value* result = builder.fold_resize_x(shifted, Type::Bool);
    unittest_assert(dynamic_cast<LtSInst*>(result));
    unittest_assert(((LtSInst*) result)->arg(0) == input);
    Value* zero = ((LtSInst*) result)->arg(1);
    check_const(zero, type, 0);
    unittest_assert(result->type() == Type::Bool);
  }
}

void test_fold_resize_x_sign_bit_negative() {
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* block = builder.build_block({type, type});
    builder.move_to_end(block);
    Value* input = block->arg(0);
    for (uint64_t amount : {uint64_t(type_width(type) - 2), uint64_t(type_width(type)), uint64_t(type_width(type) + 1)}) {
      Value* shifted = builder.build_shr_u(input, builder.build_const(type, amount));
      Value* result = builder.fold_resize_x(shifted, Type::Bool);
      unittest_assert(dynamic_cast<ResizeXInst*>(result));
      unittest_assert(((ResizeXInst*) result)->arg(0) == shifted);
    }
    Value* variable_shift = builder.build_shr_u(input, block->arg(1));
    Value* variable_result = builder.fold_resize_x(variable_shift, Type::Bool);
    unittest_assert(dynamic_cast<ResizeXInst*>(variable_result));
    unittest_assert(((ResizeXInst*) variable_result)->arg(0) == variable_shift);
    Value* shifted = builder.build_shr_u(input, builder.build_const(type, type_width(type) - 1));
    Type target = type == Type::Int8 ? Type::Int16 : Type::Int8;
    Value* result = builder.fold_resize_x(shifted, target);
    unittest_assert(dynamic_cast<ResizeXInst*>(result));
    unittest_assert(((ResizeXInst*) result)->arg(0) == shifted);
    Value* signed_shift = builder.build_shr_s(input, builder.build_const(type, type_width(type) - 1));
    Value* signed_result = builder.fold_resize_x(signed_shift, Type::Bool);
    unittest_assert(dynamic_cast<ResizeXInst*>(signed_result));
    unittest_assert(((ResizeXInst*) signed_result)->arg(0) == signed_shift);
  }
}

void test_fold_masked_resize_roundtrip() {
  const uint64_t masks[] = {0, 1, 0x55, 0xff00, 0x123456789abcdef0,
                            uint64_t(1) << 63, ~uint64_t(0)};
  for (Type source : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    for (Type intermediate : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
      if (source == intermediate) {
        continue;
      }
      Context context;
      Allocator allocator;
      Section section(context, allocator);
      Builder builder(&section);
      Block* block = builder.build_block({source, intermediate});
      builder.move_to_end(block);
      Value* x = block->arg(0);
      Value* resized = builder.build_resize_x(x, intermediate);
      for (uint64_t mask : masks) {
        Const* constant = builder.build_const(intermediate, mask & type_mask(intermediate));
        for (Value* masked : {builder.build_and(resized, constant), builder.build_and(constant, resized)}) {
          Value* result = builder.fold_resize_u(masked, source);
          uint64_t expected = mask & type_mask(intermediate) & type_mask(source);
          if (expected == 0) {
            check_const(result, source, 0);
          } else if (expected == type_mask(source)) {
            unittest_assert(result == x);
          } else {
            unittest_assert(dynamic_cast<AndInst*>(result));
            AndInst* and_inst = (AndInst*) result;
            unittest_assert(and_inst->arg(0) == x);
            check_const(and_inst->arg(1), source, expected);
          }
          unittest_assert(result->type() == source);
        }
      }
      Value* dynamic_mask = builder.build_and(resized, block->arg(1));
      Value* result = builder.fold_resize_u(dynamic_mask, source);
      unittest_assert(dynamic_cast<ResizeUInst*>(result));
      unittest_assert(((ResizeUInst*) result)->arg(0) == dynamic_mask);
    }
  }
}

void test_fold_masked_resize_without_roundtrip() {
  Context context;
  Allocator allocator;
  Section section(context, allocator);
  Builder builder(&section);
  Block* block = builder.build_block({Type::Int32});
  builder.move_to_end(block);
  Value* resized = builder.build_resize_x(block->arg(0), Type::Int8);
  Value* masked = builder.build_and(resized, builder.build_const(Type::Int8, 7));
  Value* result = builder.fold_resize_u(masked, Type::Int64);
  unittest_assert(dynamic_cast<ResizeUInst*>(result));
  unittest_assert(((ResizeUInst*) result)->arg(0) == masked);
}

void check_cse_operand_order(const std::string& opcode, Type type,
                             bool commutative, bool constant = false) {
  Context context;
  Allocator allocator;
  std::ostringstream text;
  text << "section {\nb0(%0: Ptr, %1: " << type << ", %2: " << type << "):\n";
  std::string rhs = constant ? "1:" + std::string(to_string(type)) : "%2";
  text << "  %3 = " << opcode << " %1, " << rhs << "\n";
  text << "  %4 = " << opcode << " " << rhs << ", %1\n";
  text << "  Store %0, %3, aliasing=0, offset=0\n";
  text << "  Store %0, %4, aliasing=0, offset=8\n  Exit\n}\n";
  std::istringstream stream(text.str());
  Section* section = SectionReader<Builder>::read_section(context, allocator, stream);
  std::vector<StoreInst*> outputs;
  for (Inst* inst : *section->entry()) {
    if (dynmatch(StoreInst, store, inst)) {
      outputs.push_back(store);
    }
  }
  Inst* first = (Inst*) outputs.at(0)->value();
  Inst* second = (Inst*) outputs.at(1)->value();
  unittest_assert(first->is_commutative() == commutative);
  unittest_assert(second->is_commutative() == commutative);
  Value* left = first->arg(0);
  Value* right = first->arg(1);
  unittest_assert(!first->equals(second));
  unittest_assert(!section->verify(std::cout));
  CommonSubexprElim::run(section);
  unittest_assert(!section->verify(std::cout));
  unittest_assert((outputs.at(0)->value() == outputs.at(1)->value()) == commutative);
  unittest_assert(first->arg(0) == left && first->arg(1) == right);
  unittest_assert(!first->equals(second));
  delete section;
}

void test_cse_commutative_operands() {
  for (const std::string& opcode : {"Add", "Mul", "And", "Or", "Xor", "Eq"}) {
    for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
      check_cse_operand_order(opcode, type, true);
      check_cse_operand_order(opcode, type, true, true);
    }
  }
  for (const std::string& opcode : {"And", "Or", "Xor", "Eq"}) {
    check_cse_operand_order(opcode, Type::Bool, true);
    check_cse_operand_order(opcode, Type::Bool, true, true);
  }
  check_cse_operand_order("Eq", Type::Ptr, true);
}

void test_cse_ordered_operands() {
  for (const std::string& opcode : {"Sub", "DivS", "DivU", "ModS", "ModU",
                                   "Shl", "ShrS", "ShrU", "LtS", "LtU"}) {
    for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
      check_cse_operand_order(opcode, type, false);
      check_cse_operand_order(opcode, type, false, true);
    }
  }
  for (const std::string& opcode : {"AddF", "SubF", "MulF", "DivF", "LtFO", "LtFU"}) {
    for (Type type : {Type::Float32, Type::Float64}) {
      check_cse_operand_order(opcode, type, false);
    }
  }
}

void test_cse_commutative_after_substitution() {
  Context context;
  Allocator allocator;
  Section section(context, allocator);
  Builder builder(&section);
  Block* entry = builder.build_block({Type::Ptr, Type::Int32, Type::Int32});
  builder.move_to_end(entry);
  Value* x = entry->arg(1);
  Value* y = entry->arg(2);
  Value* a = builder.build_add(x, y);
  Value* b = builder.build_add(y, x);
  Value* first = builder.build_xor(a, x);
  Value* second = builder.build_xor(x, b);
  StoreInst* output1 = builder.build_store(entry->arg(0), first, AliasingGroup(0), 0);
  StoreInst* output2 = builder.build_store(entry->arg(0), second, AliasingGroup(0), 8);
  builder.build_exit();
  section.order_blocks(BlockOrdering::Dominator);
  CommonSubexprElim::run(&section);
  unittest_assert(!section.verify(std::cout));
  unittest_assert(output1->value() == output2->value());
}

void test_rule_dsl_builder_integration() {
  Context context;
  Allocator allocator;
  Section section(context, allocator);
  Builder builder(&section);
  Block* block = builder.build_block({Type::Int8, Type::Int8, Type::Bool, Type::Ptr});
  builder.move_to_end(block);
  Value* x = block->arg(0);
  Value* y = block->arg(1);
  Value* cond = block->arg(2);
  Value* pointer = block->arg(3);
  Value* inner = builder.build_xor(x, y);
  unittest_assert(builder.fold_xor(inner, y) == x);
  TraceBuilder trace(&section);
  trace.move_to_end(block);
  unittest_assert(trace.build_xor(inner, y) == x);
  unittest_assert(trace.build_select(cond, pointer, pointer) == pointer);
  Const* pointer_eq = dynamic_cast<Const*>(builder.fold_eq(pointer, pointer));
  unittest_assert(pointer_eq && pointer_eq->type() == Type::Bool && pointer_eq->value() == 1);
  Value* selected = builder.build_select(cond, builder.build_const(Type::Int8, 3),
                                       builder.build_const(Type::Int8, 5));
  SelectInst* sum = dynamic_cast<SelectInst*>(trace.build_add(selected, builder.build_const(Type::Int8, 2)));
  unittest_assert(sum && sum->cond() == cond);
  Const* true_value = dynamic_cast<Const*>(sum->arg(1));
  Const* false_value = dynamic_cast<Const*>(sum->arg(2));
  unittest_assert(true_value && true_value->value() == 5);
  unittest_assert(false_value && false_value->value() == 7);
  SelectInst* reversed_sum = dynamic_cast<SelectInst*>(builder.fold_add(builder.build_const(Type::Int8, 2), selected));
  unittest_assert(reversed_sum && reversed_sum->cond() == cond);
  unittest_assert(builder.is_always_equal(reversed_sum->arg(1), true_value));
  unittest_assert(builder.is_always_equal(reversed_sum->arg(2), false_value));
  for (Type type : {Type::Float32, Type::Float64, Type::Ptr}) {
    Value* value = builder.build_poison(type);
    unittest_assert(builder.fold_select(cond, value, value) == value);
    unittest_assert(builder.fold_select(builder.build_const(Type::Bool, 1), value,
                                        builder.build_const(type, 0)) == value);
    Const* equal = dynamic_cast<Const*>(builder.fold_eq(value, value));
    unittest_assert(equal && equal->value() == 1);
  }
  for (bool unspecified : {false, true}) {
    Value* resized = unspecified ? builder.fold_resize_x(selected, Type::Int64)
                                : builder.fold_resize_u(selected, Type::Int64);
    SelectInst* resized_select = dynamic_cast<SelectInst*>(resized);
    unittest_assert(resized_select && resized_select->cond() == cond);
    unittest_assert(resized_select->type() == Type::Int64);
    unittest_assert(builder.is_always_equal(resized_select->arg(1), builder.build_const(Type::Int64, 3)));
    unittest_assert(builder.is_always_equal(resized_select->arg(2), builder.build_const(Type::Int64, 5)));
  }
  Const* widened = dynamic_cast<Const*>(builder.fold_resize_x(builder.build_const(Type::Int8, 255), Type::Int64));
  unittest_assert(widened && widened->type() == Type::Int64 && widened->value() == 255);
  builder.build_exit();
  unittest_assert(!section.verify(std::cout));
}

void check_fold_constant_select(Value* result, Value* cond, Type type, uint64_t t, uint64_t f) {
  t &= type_mask(type);
  f &= type_mask(type);
  if (dynamic_cast<Const*>(result)) {
    check_const(result, type, t);
    unittest_assert(t == f);
  } else {
    SelectInst* select = dynamic_cast<SelectInst*>(result);
    unittest_assert(select && select->cond() == cond && select->type() == type);
    check_const(select->arg(1), type, t);
    check_const(select->arg(2), type, f);
  }
}

void test_fold_constant_select_propagation() {
  for (Type type : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* block = builder.build_block({Type::Bool, Type::Bool});
    builder.move_to_end(block);
    Value* cond = block->arg(0);
    Value* other_cond = block->arg(1);
    uint64_t mask = type_mask(type);
    Value* left = builder.build_select(cond, builder.build_const(type, mask), builder.build_const(type, 5));
    Value* right = builder.build_select(cond, builder.build_const(type, 3), builder.build_const(type, 9));
    Value* unrelated = builder.build_select(other_cond, builder.build_const(type, 3), builder.build_const(type, 9));
    Value* constant = builder.build_const(type, 7);
    for (bool reversed : {false, true}) {
      Value* a = reversed ? constant : left;
      Value* b = reversed ? left : constant;
      check_fold_constant_select(builder.fold_and(a, b), cond, type, mask & 7, 5 & 7);
      check_fold_constant_select(builder.fold_or(a, b), cond, type, mask | 7, 5 | 7);
      check_fold_constant_select(builder.fold_xor(a, b), cond, type, mask ^ 7, 5 ^ 7);
      if (type != Type::Bool) {
        check_fold_constant_select(builder.fold_add(a, b), cond, type, mask + 7, 12);
        check_fold_constant_select(builder.fold_sub(a, b), cond, type,
                                  reversed ? 7 - mask : mask - 7, reversed ? 2 : uint64_t(5) - 7);
        check_fold_constant_select(builder.fold_mul(a, b), cond, type, mask * 7, 35);
      }
    }
    check_fold_constant_select(builder.fold_and(left, right), cond, type, mask & 3, 5 & 9);
    check_fold_constant_select(builder.fold_or(left, right), cond, type, mask | 3, 5 | 9);
    check_fold_constant_select(builder.fold_xor(left, right), cond, type, mask ^ 3, 5 ^ 9);
    if (type != Type::Bool) {
      check_fold_constant_select(builder.fold_add(left, right), cond, type, mask + 3, 14);
      check_fold_constant_select(builder.fold_sub(left, right), cond, type, mask - 3, uint64_t(5) - 9);
      check_fold_constant_select(builder.fold_mul(left, right), cond, type, mask * 3, 45);
      unittest_assert(dynamic_cast<AddInst*>(builder.fold_add(left, unrelated)));
      unittest_assert(dynamic_cast<SubInst*>(builder.fold_sub(left, unrelated)));
      unittest_assert(dynamic_cast<MulInst*>(builder.fold_mul(left, unrelated)));
      unittest_assert(dynamic_cast<AndInst*>(builder.fold_and(left, unrelated)));
      unittest_assert(dynamic_cast<OrInst*>(builder.fold_or(left, unrelated)));
      unittest_assert(dynamic_cast<XorInst*>(builder.fold_xor(left, unrelated)));
    }
    builder.build_exit();
    unittest_assert(!section.verify(std::cout));
  }
}

void test_fold_select_unselected_poison() {
  for (bool true_arm : {false, true}) {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* block = builder.build_block({Type::Ptr, Type::Bool});
    builder.move_to_end(block);
    Value* cond = block->arg(1);
    Value* poison = builder.build_poison(Type::Bool);
    Value* result = true_arm ? builder.fold_select(cond, cond, poison)
                             : builder.fold_select(cond, poison, cond);
    unittest_assert(dynamic_cast<SelectInst*>(result));
    builder.build_store(block->arg(0), result, 0, 0);
    builder.build_exit();
    unittest_assert(!section.verify(std::cout));
    uint8_t output = 42;
    Interpreter interpreter(&section, {Interpreter::Bits::constant(&output), Interpreter::Bits::constant(true_arm)});
    unittest_assert(interpreter.run() == Interpreter::Event::Exit);
    unittest_assert(output == uint8_t(true_arm));
  }
}

int main(int argc, char** argv) {
  metajit::LLVMCodeGen::initilize_llvm_jit();

  DiffTestSuite suite("tests/output/test_opt", argc, argv);

  suite.test("rule DSL builder integration").run(test_rule_dsl_builder_integration);
  suite.test("fold constant select propagation").run(test_fold_constant_select_propagation);
  suite.test("fold mod u power of two").run(test_fold_mod_u_power_of_two);
  suite.test("fold select unselected poison").run(test_fold_select_unselected_poison);

  suite.test("cse commutative operands").run(test_cse_commutative_operands);
  suite.test("cse ordered operands").run(test_cse_ordered_operands);
  suite.test("cse commutative after substitution").run(test_cse_commutative_after_substitution);
  suite.test("fold xor same operand").run(test_fold_xor_same_operand);
  suite.test("fold lt_u same operand").run(test_fold_lt_u_same_operand);
  suite.test("fold lt_s same operand").run(test_fold_lt_s_same_operand);
  suite.test("fold and repeated operand").run(test_fold_and_repeated_operand);
  suite.test("fold or negated or operand").run(test_fold_or_negated_or_operand);
  suite.test("fold or xor operand").run(test_fold_or_xor_operand);
  suite.test("fold or repeated operand").run(test_fold_or_repeated_operand);
  suite.test("fold unmasked shift pairs").run(test_fold_unmasked_shift_pairs);
  suite.test("fold shift pairs int64 boundaries").run(test_fold_shift_pairs_int64_boundaries);
  suite.test("builder is always equal").run(test_builder_is_always_equal);
  suite.test("fold equal constants factoring").run(test_fold_equal_constants_factoring);
  suite.test("fold equal constants absorption").run(test_fold_equal_constants_absorption);
  suite.test("fold equal constants complement absorption").run(test_fold_equal_constants_complement_absorption);
  suite.test("fold equal constants repeated_or").run(test_fold_equal_constants_repeated_or);
  suite.test("fold equal constants xor_or").run(test_fold_equal_constants_xor_or);
  suite.test("fold equal constants negated_or").run(test_fold_equal_constants_negated_or);
  suite.test("fold equal constants select").run(test_fold_equal_constants_select);
  suite.test("fold or factoring").run(test_fold_or_factoring);
  suite.test("fold and factoring").run(test_fold_and_factoring);
  suite.test("fold and mixed absorption").run(test_fold_and_mixed_absorption);
  suite.test("fold or mixed absorption").run(test_fold_or_mixed_absorption);
  suite.test("fold or absorption").run(test_fold_or_absorption);
  suite.test("fold or complement absorption").run(test_fold_or_complement_absorption);
  suite.test("fold and complement absorption").run(test_fold_and_complement_absorption);
  suite.test("fold and xor constant").run(test_fold_and_xor_constant);
  suite.test("fold and xor subset").run(test_fold_and_xor_subset);
  suite.test("fold and absorption").run(test_fold_and_absorption);
  suite.test("fold unspecified extension roundtrip").run(test_fold_unspecified_extension_roundtrip);
  suite.test("fold narrowing resize uu").run(test_fold_narrowing_resize_uu);
  suite.test("fold narrowing resize ux").run(test_fold_narrowing_resize_ux);
  suite.test("fold narrowing resize xu").run(test_fold_narrowing_resize_xu);
  suite.test("fold nested unsigned resizes").run(test_fold_nested_unsigned_resizes);
  suite.test("fold unsigned extension roundtrip").run(test_fold_unsigned_extension_roundtrip);
  suite.test("fold nested narrowing resize x").run(test_fold_nested_narrowing_resize_x);
  suite.test("fold resize x sign bit").run(test_fold_resize_x_sign_bit);
  suite.test("fold resize x sign bit negative").run(test_fold_resize_x_sign_bit_negative);
  suite.test("fold masked resize roundtrip").run(test_fold_masked_resize_roundtrip);
  suite.test("fold masked resize without roundtrip").run(test_fold_masked_resize_without_roundtrip);
  suite.test("fold add complement to sub").run(test_fold_add_complement_to_sub);
  suite.test("fold sub eq zero").run(test_fold_sub_eq_zero);
  suite.test("fold mul power of two").run(test_fold_mul_power_of_two);
  suite.test("fold mul power of two negative").run(test_fold_mul_power_of_two_negative);
  suite.test("fold sub same operand").run(test_fold_sub_same_operand);
  suite.test("simplify overwritten bytes").run(test_simplify_overwritten_bytes);
  suite.test("simplify used bits other use").run(test_simplify_used_bits_other_use);
  suite.test("simplify used bits effects").run(test_simplify_used_bits_effects);
  suite.test("simplify used bits shl mask").run(test_simplify_used_bits_shl_mask);
  suite.test("simplify used bits load address").run(test_simplify_used_bits_load_address);
  suite.test("simplify used bits alloca size").run(test_simplify_used_bits_alloca_size);
  suite.diff_test("simplify overwritten bytes differential").run(test_simplify_overwritten_bytes_differential);

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
    Simplify::run(&section, 4);
    unittest_assert(!section.verify(std::cout));
    unittest_assert(frozen_store->value() == frozen);
    unittest_assert(masked_store->value() == masked);
    unittest_assert(masked->arg(0) == frozen);
  });

  suite.test("natural block order deep chain").run(test_natural_order_deep_chain);

  suite.diff_test("resize_resize_to_mask").run([](Builder& builder, TestData& data) {

    Value* input = data.input(Type::Int64);
    Value* smaller = builder.fold_resize_x(input, Type::Int8);
    Value* wide = builder.fold_resize_u(smaller, Type::Int64);
    data.output(wide);

    // it's really the smart constructors that do this.
    // the ResizeX would be removed by DeadCodeElim
    check_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int64, flags={}, aliasing=0, offset=0
  %2 = And %1, 255:Int64
  Store %0, %2, aliasing=0, offset=8
}
)", builder.section());
  });

  suite.diff_test("shru_and_shl_to_and").run([](Builder& builder, TestData& data) {

    Value* input = data.input(Type::Int64);
    Value* shifted = builder.fold_shr_u(input, builder.build_const(Type::Int64, 1));
    Value* anded = builder.fold_and(shifted, builder.build_const(Type::Int64, 3));
    Value* back = builder.fold_shl(anded, builder.build_const(Type::Int64, 1));
    data.output(back);

    // it's really the smart constructors that do this.
    check_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int64, flags={}, aliasing=0, offset=0
  %2 = And %1, 6:Int64
  Store %0, %2, aliasing=0, offset=8
}
)", builder.section());
  });

  suite.diff_test("shru_and_shl_to_shr").run([](Builder& builder, TestData& data) {

    Value* input = data.input(Type::Int64);
    Value* shifted = builder.fold_shr_u(input, builder.build_const(Type::Int64, 10));
    Value* anded = builder.fold_and(shifted, builder.build_const(Type::Int64, 1));
    Value* back = builder.fold_shl(anded, builder.build_const(Type::Int64, 1));
    data.output(back);

    // it's really the smart constructors that do this.
    check_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int64, flags={}, aliasing=0, offset=0
  %2 = ShrU %1, 9:Int64
  %3 = And %2, 2:Int64
  Store %0, %3, aliasing=0, offset=8
}
)", builder.section());
  });

  suite.diff_test("shru_and_shl_to_shl").run([](Builder& builder, TestData& data) {

    Value* input = data.input(Type::Int64);
    Value* shifted = builder.fold_shr_u(input, builder.build_const(Type::Int64, 1));
    Value* anded = builder.fold_and(shifted, builder.build_const(Type::Int64, 1));
    Value* back = builder.fold_shl(anded, builder.build_const(Type::Int64, 3));
    data.output(back);

    check_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int64, flags={}, aliasing=0, offset=0
  %2 = Shl %1, 2:Int64
  %3 = And %2, 8:Int64
  Store %0, %3, aliasing=0, offset=8
}
)", builder.section());
  });


  suite.diff_test("shl_and_shl_to_shl").run([](Builder& builder, TestData& data) {

    Value* input = data.input(Type::Int64);
    Value* shifted = builder.fold_shl(input, builder.build_const(Type::Int64, 1));
    Value* anded = builder.fold_and(shifted, builder.build_const(Type::Int64, 0xff));
    Value* back = builder.fold_shl(anded, builder.build_const(Type::Int64, 3));
    data.output(back);

    check_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int64, flags={}, aliasing=0, offset=0
  %2 = Shl %1, 4:Int64
  %3 = And %2, 2040:Int64
  Store %0, %3, aliasing=0, offset=8
}
)", builder.section());
  });

  suite.diff_test("or_and_and_to_or").run([](Builder& builder, TestData& data) {
    Value* input = data.input(Type::Int64);
    Value* part1 = builder.fold_and(input, builder.build_const(Type::Int64, 3));
    Value* part2 = builder.fold_and(input, builder.build_const(Type::Int64, 12));
    Value* result = builder.fold_or(part1, part2);
    data.output(result);

    check_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int64, flags={}, aliasing=0, offset=0
  %2 = And %1, 15:Int64
  Store %0, %2, aliasing=0, offset=8
}
)", builder.section());
  });

  suite.diff_test("and_or_and_shortcut").run([](Builder& builder, TestData& data) {
    Value* input1 = data.input(Type::Int64);
    Value* input2 = data.input(Type::Int64);
    Value* part1 = builder.fold_and(input1, builder.build_const(Type::Int64, 0xff00));
    Value* part2 = builder.fold_or(input2, part1);
    Value* result = builder.fold_and(part2, builder.build_const(Type::Int64, 0xffff00ff));
    data.output(result);

    check_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int64, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int64, flags={}, aliasing=0, offset=8
  %3 = And %2, 4294902015:Int64
  Store %0, %3, aliasing=0, offset=16
}
)", builder.section());
  });

  for (bool nested_true : {false, true}) {
    suite.diff_test(nested_true ? "fold_select_nested_true" : "fold_select_nested_false").run([nested_true](Builder& builder, TestData& data) {
      Value* cond = data.input(Type::Bool);
      Value* zero = builder.build_const(Type::Int8, 0);
      Value* one = builder.build_const(Type::Int8, 1);
      Value* two = builder.build_const(Type::Int8, 2);
      Value* four = builder.build_const(Type::Int8, 4);
      Value* inner = nested_true
        ? builder.build_select(cond, four, one)
        : builder.build_select(cond, one, four);
      Value* middle = nested_true
        ? builder.build_select(cond, inner, two)
        : builder.build_select(cond, two, inner);
      Value* result = nested_true
        ? builder.fold_select(cond, middle, zero)
        : builder.fold_select(cond, zero, middle);
      auto* select = dynamic_cast<SelectInst*>(result);
      unittest_assert(select);
      unittest_assert(select->cond() == cond);
      unittest_assert(select->arg(1) == (nested_true ? four : zero));
      unittest_assert(select->arg(2) == (nested_true ? zero : four));
      data.output(result);
    });
  }

  suite.diff_test("select_and_knownbits").run([](Builder& builder, TestData& data) {

    Value* cond = data.input(Type::Bool);
    Value* value = data.input(Type::Int64);

    // knownbits optimizes this condition to false
    Value* select_cond = builder.build_and(
      builder.build_resize_u(cond, Type::Int64),
      builder.build_const(Type::Int64, 2)
    );
    Value* select = builder.build_select(builder.build_resize_u(select_cond, Type::Bool), builder.build_const(Type::Int64, 0), value);
    data.output(select);

    check_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int64, flags={}, aliasing=0, offset=8
  Store %0, %2, aliasing=0, offset=16
}
)", builder.section());
  });

  suite.diff_test("and_idempotent_not_constant").run([](Builder& builder, TestData& data) {
    Value* in1 = data.input(Type::Int8);
    Value* in2 = data.input(Type::Int8);
    Value* x = builder.build_and(in1, builder.build_const(Type::Int8, 0b11110000));
    // (x = ____0000) & (y = 1111____) -> x
    Value* y = builder.build_or(in2, builder.build_const(Type::Int8, 0b11110000));
    data.output(builder.build_and(x, y));
    check_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = And %1, 240:Int8
  Store %0, %3, aliasing=0, offset=2
}
)", builder.section());
  });

  suite.diff_test("fold_and_nested_constants").run([](Builder& builder, TestData& data) {
    Value* input = data.input(Type::Int8);
    Value* inner = builder.build_and(input, builder.build_const(Type::Int8, 0xf0));
    for (uint64_t mask : {0x3c, 0x0f}) {
      Value* constant = builder.build_const(Type::Int8, mask);
      for (bool swapped : {false, true}) {
        Value* result = swapped
          ? builder.fold_and(constant, inner)
          : builder.fold_and(inner, constant);
        if (mask == 0x0f) {
          auto* zero = dynamic_cast<Const*>(result);
          unittest_assert(zero && zero->value() == 0);
        } else {
          auto* and_inst = dynamic_cast<AndInst*>(result);
          unittest_assert(and_inst && and_inst->arg(0) == input);
          auto* folded_mask = dynamic_cast<Const*>(and_inst->arg(1));
          unittest_assert(folded_mask && folded_mask->value() == 0x30);
        }
        data.output(result);
      }
    }
  });

  suite.diff_test("or_idempotent").run([](Builder& builder, TestData& data) {
    Value* value = data.input(Type::Int16);
    Value* value2 = builder.build_or(value, builder.build_const(Type::Int16, 0b11));
    // the second or is unnecessary
    Value* value3 = builder.build_or(value2, builder.build_const(Type::Int16, 0b11));
    data.output(value3);
    check_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int16, flags={}, aliasing=0, offset=0
  %2 = Or %1, 3:Int16
  Store %0, %2, aliasing=0, offset=2
}
)", builder.section());
  });

  suite.diff_test("or_and_one_component").run([](Builder& builder, TestData& data) {
    Value* value1 = data.input(Type::Int16);
    Value* value2 = data.input(Type::Int16);
    Value* value3 = data.input(Type::Int16);
    Value* masked = builder.fold_and(value3, builder.build_const(Type::Int16, 0xf000));
    Value* shifted = builder.fold_shl(value2, builder.build_const(Type::Int16, 14));
    Value* orop1 = builder.fold_or(masked, value1);
    Value* orop2 = builder.fold_or(orop1, shifted);
    data.output(builder.fold_and(orop2, builder.build_const(Type::Int16, 0xfff)));
    check_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int16, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int16, flags={}, aliasing=0, offset=2
  %3 = Load %0, type=Int16, flags={}, aliasing=0, offset=4
  %4 = And %1, 4095:Int16
  Store %0, %4, aliasing=0, offset=6
}
)", builder.section());
  });

  suite.diff_test("or_usedbits").run([](Builder& builder, TestData& data) {
    Value* value1 = data.input(Type::Int16);
    Value* value2 = builder.build_resize_s(value1, Type::Int32);
    Value* value3 = builder.build_or(value2, builder.build_const(Type::Int32, 0xffff0000));
    data.output(value3);
    check_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int16, flags={}, aliasing=0, offset=0
  %2 = ResizeX %1, type=Int32
  %3 = Or %2, 4294901760:Int32
  Store %0, %3, aliasing=0, offset=4
}
)", builder.section());
  });

  suite.diff_test("simplify removes idempotent And Or ops").run(test_and_or_idempotence_simplify);

  // tests for SimplifyCFG
  suite.test("simplifycfg preserves externally used block arguments").run([]() {
    Context context;
    Allocator allocator;
    std::istringstream input(R"(section {
b0(%0: Ptr, %1: Bool):
    Branch %1, true_block=b1, false_block=b2
b1:
    Jump 1:Int64, block=b3
b2:
    Jump 0:Int64, block=b3
b3(%2: Int64):
    Jump block=b4
b4:
    Branch 0:Bool, true_block=b4, false_block=b5
b5:
    Store %0, %2, aliasing=0, offset=0
    Exit
})");
    Section* section = SectionReader<>::read_section(context, allocator, input);
    section->set_ordering(BlockOrdering::Natural);
    unittest_assert(!section->verify(std::cerr));
    SimplifyCFG::run(section);
    unittest_assert(!section->verify(std::cerr));
    for (size_t input = 0; input < 2; input++) {
      uint64_t output = 0;
      Interpreter interpreter(section, {
        Interpreter::Bits::constant(Type::Ptr, (uint64_t) &output),
        Interpreter::Bits::constant(Type::Bool, input)
      });
      interpreter.run_until(Interpreter::Event::Exit);
      unittest_assert(output == input);
    }
    delete section;
  });

  suite.diff_test("simplifycfg unreachable blocks").run([](Builder& builder, TestData& data) {
    Block* unreachable1 = builder.build_block();
    Block* unreachable2 = builder.build_block();

    data.output(builder.build_const(Type::Int64, 42));
    builder.build_exit();

    builder.move_to_end(unreachable1);
    builder.build_jump(unreachable2);
    builder.move_to_end(unreachable2);
    builder.build_exit();

    check_simplifycfg(R"(section {
b0(%0: Ptr):
  Store %0, 42:Int64, aliasing=0, offset=0
  Exit
}
)", builder.section());
  });
  suite.diff_test("simplifycfg branch with const true").run([](Builder& builder, TestData& data) {
    Value* cond = builder.build_const(Type::Bool, 1);
    Block* then_block = builder.build_block();
    Block* else_block = builder.build_block();
    Block* merge_block = builder.build_block();

    builder.build_branch(cond, then_block, else_block);
    builder.move_to_end(then_block);
    builder.build_jump(merge_block);
    builder.move_to_end(else_block);
    builder.build_jump(merge_block);
    builder.move_to_end(merge_block);
    data.output(builder.build_const(Type::Int64, 42));
    builder.build_exit();

    check_simplifycfg(R"(section {
b0(%0: Ptr):
  Store %0, 42:Int64, aliasing=0, offset=0
  Exit
}
)", builder.section());
  });

  suite.diff_test("simplifycfg branch with both targets same").run([](Builder& builder, TestData& data) {
    Value* cond = data.input(Type::Bool);
    Block* then_block = builder.build_block();

    builder.build_branch(cond, then_block, then_block);
    builder.move_to_end(then_block);
    data.output(builder.build_const(Type::Int64, 42));
    builder.build_exit();

    check_simplifycfg(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  Store %0, 42:Int64, aliasing=0, offset=8
  Exit
}
)", builder.section());
    });

    suite.diff_test("simplifycfg jump-threading with args").run([](Builder& builder, TestData& data) {
    Value* cond1 = data.input(Type::Bool);
    Value* cond2 = data.input(Type::Bool);

    Block* b1 = builder.build_block();
    Block* b1_false = builder.build_block();
    Block* b1_true = builder.build_block();
    Block* b1_false_local = builder.build_block();
    Block* b2 = builder.build_block();
    Block* pass_through = builder.build_block({Type::Int64});
    Block* final_target = builder.build_block({Type::Int64});

    builder.build_branch(cond1, b1, b1_false);

    builder.move_to_end(b1_false);
    builder.build_jump(final_target, {builder.build_const(Type::Int64, 3)});

    builder.move_to_end(b1);
    builder.build_branch(cond2, b1_true, b1_false_local);

    builder.move_to_end(b1_true);
    builder.build_jump(b2);

    builder.move_to_end(b1_false_local);
    builder.build_jump(pass_through, {builder.build_const(Type::Int64, 1)});

    builder.move_to_end(b2);
    builder.build_jump(pass_through, {builder.build_const(Type::Int64, 2)});

    builder.move_to_end(pass_through);
    builder.build_jump(final_target, {pass_through->arg(0)});

    builder.move_to_end(final_target);
    data.output(final_target->arg(0));
    builder.build_exit();

    // pass_through should be bypassed.
    check_simplifycfg(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Bool, flags={}, aliasing=0, offset=1
  Branch %1, true_block=b1, false_block=b4
b1:
  Branch %2, true_block=b2, false_block=b3
b2:
  Jump 2:Int64, block=b5
b3:
  Jump 1:Int64, block=b5
b4:
  Jump 3:Int64, block=b5
b5(%8: Int64):
  Store %0, %8, aliasing=0, offset=8
  Exit
}
)", builder.section());
    });

    suite.diff_test("simplifycfg jump with args further substs").run([](Builder& builder, TestData& data) {
    Value* cond = builder.build_const(Type::Bool, 0);
    Value* input = data.input(Type::Int64);
    Block* then_block = builder.build_block();
    Block* else_block = builder.build_block();
    Block* merge_block = builder.build_block({Type::Int64});

    builder.build_branch(cond, then_block, else_block);
    builder.move_to_end(then_block);
    builder.build_jump(merge_block, {builder.build_const(Type::Int64, 42)});
    builder.move_to_end(else_block);
    builder.build_jump(merge_block, {input});
    builder.move_to_end(merge_block);
    data.output(merge_block->arg(0));
    builder.build_exit();
    check_simplifycfg(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int64, flags={}, aliasing=0, offset=0
  Store %0, %1, aliasing=0, offset=8
  Exit
}
)", builder.section());
  });

  suite.diff_test("simplifycfg invalidates dominator order").run([](Builder& builder, TestData& data) {
    Value* cond1 = data.input(Type::Bool);
    Value* input = data.input(Type::Int64);
    Block* branch_block = builder.build_block();
    Block* merge_block = builder.build_block({Type::Int64});
    Block* return_block = builder.build_block();
    Block* then_block = builder.build_block();
    Block* else_block = builder.build_block();

    builder.build_branch(cond1, branch_block, then_block);

    builder.move_to_end(branch_block);
    Value* cond = builder.build_const(Type::Bool, 1);
    builder.build_branch(cond, then_block, else_block);

    builder.move_to_end(merge_block);
    builder.build_jump(return_block);
    builder.move_to_end(return_block);
    data.output(merge_block->arg(0));
    builder.build_exit();
    builder.move_to_end(then_block);
    builder.build_jump(merge_block, {builder.build_const(Type::Int64, 42)});
    builder.move_to_end(else_block);
    builder.build_jump(merge_block, {input});
    builder.section()->set_ordering(BlockOrdering::Dominator);
    check_simplifycfg(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int64, flags={}, aliasing=0, offset=8
  Store %0, 42:Int64, aliasing=0, offset=16
  Exit
}
)", builder.section());
  });

  suite.diff_test("simplifycfg missing substs").run([](Builder& builder, TestData& data) {
    Value* cond1 = data.input(Type::Bool);
    Value* input = data.input(Type::Int64);
    Block* branch_block = builder.build_block();
    Block* merge_block = builder.build_block({Type::Int64});
    Block* return_block = builder.build_block();
    Block* then_block = builder.build_block();
    Block* else_block = builder.build_block();
    Block* then_block2 = builder.build_block();
    Block* else_block2 = builder.build_block();

    builder.build_branch(cond1, branch_block, then_block);

    builder.move_to_end(branch_block);
    Value* cond = builder.build_const(Type::Bool, 1);
    builder.build_branch(cond, then_block, else_block);

    builder.move_to_end(merge_block);
    Value* cond2 = data.input(Type::Bool);
    builder.build_branch(cond2, then_block2, else_block2);
    builder.move_to_end(then_block2);
    data.output(data.input(Type::Bool));
    data.output(cond2);
    builder.build_jump(return_block);
    builder.move_to_end(else_block2);
    data.output(cond2);
    builder.build_jump(return_block);
    builder.move_to_end(return_block);
    data.output(merge_block->arg(0));
    builder.build_exit();
    builder.move_to_end(then_block);
    builder.build_jump(merge_block, {builder.build_const(Type::Int64, 42)});
    builder.move_to_end(else_block);
    builder.build_jump(merge_block, {input});
    builder.section()->set_ordering(BlockOrdering::Dominator);
    check_simplifycfg(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int64, flags={}, aliasing=0, offset=8
  %3 = Load %0, type=Bool, flags={}, aliasing=0, offset=16
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = Load %0, type=Bool, flags={}, aliasing=0, offset=17
  Store %0, %5, aliasing=0, offset=18
  Store %0, 1:Bool, aliasing=0, offset=19
  Jump block=b3
b2:
  Store %0, 0:Bool, aliasing=0, offset=20
  Jump block=b3
b3:
  Store %0, 42:Int64, aliasing=0, offset=24
  Exit
}
)", builder.section());
  });

  suite.diff_test("simplifycfg jump with args further substs").run([](Builder& builder, TestData& data) {
    Value* input = data.input(Type::Int64);
    Block* arg_block = builder.build_block({Type::Int64});
    builder.build_jump(arg_block, {input});
    builder.move_to_end(arg_block);
    data.output(arg_block->arg(0));
    // now branch and use the arg after the branch
    Value* cond = data.input(Type::Bool);
    Block* then_block = builder.build_block();
    Block* else_block = builder.build_block();
    Block* merge_block = builder.build_block();
    builder.build_branch(cond, then_block, else_block);
    builder.move_to_end(then_block);
    data.output(arg_block->arg(0));
    builder.build_jump(merge_block);
    builder.move_to_end(else_block);
    data.output(arg_block->arg(0));
    builder.build_jump(merge_block);
    builder.move_to_end(merge_block);
    data.output(arg_block->arg(0));
    builder.build_exit();

    check_simplifycfg(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int64, flags={}, aliasing=0, offset=0
  Store %0, %1, aliasing=0, offset=8
  %3 = Load %0, type=Bool, flags={}, aliasing=0, offset=16
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, %1, aliasing=0, offset=24
  Jump block=b3
b2:
  Store %0, %1, aliasing=0, offset=32
  Jump block=b3
b3:
  Store %0, %1, aliasing=0, offset=40
  Exit
}
)", builder.section());
  });

  suite.diff_test("simplifycfg branch on same bool").run([](Builder& builder, TestData& data) {
    Value* cond1 = data.input(Type::Bool);
    Block* then_block = builder.build_block();
    Block* else_block = builder.build_block();
    Block* merge_block = builder.build_block();
    Block* then_then_block = builder.build_block();
    Block* then_else_block = builder.build_block();

    builder.build_branch(cond1, then_block, else_block);
    builder.move_to_end(then_block);
    builder.build_branch(cond1, then_then_block, then_else_block);
    builder.move_to_end(then_then_block);
    data.output(builder.build_const(Type::Int64, 1));
    builder.build_jump(merge_block);
    builder.move_to_end(then_else_block);
    data.output(builder.build_const(Type::Int64, 2));
    builder.build_jump(merge_block);
    builder.move_to_end(else_block);
    builder.build_jump(merge_block);
    builder.move_to_end(merge_block);
    data.output(builder.build_const(Type::Int64, 42));
    builder.build_exit();
    builder.section()->set_ordering(BlockOrdering::Dominator);

    check_simplifycfg(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  Branch %1, true_block=b1, false_block=b2
b1:
  Store %0, 1:Int64, aliasing=0, offset=8
  Jump block=b2
b2:
  Store %0, 42:Int64, aliasing=0, offset=24
  Exit
}
)", builder.section());
  });

  suite.diff_test("simplifycfg recursive substs").run([](Builder& builder, TestData& data) {
    Value* input = data.input(Type::Int64);
    Block* arg_block = builder.build_block({Type::Int64});
    builder.build_jump(arg_block, {input});
    builder.move_to_end(arg_block);
    data.output(arg_block->arg(0));
    // now branch and use the arg after the branch
    Value* cond = data.input(Type::Bool);
    Block* then_block = builder.build_block();
    Block* else_block = builder.build_block();
    Block* merge_block = builder.build_block();
    builder.build_branch(cond, then_block, else_block);
    builder.move_to_end(then_block);
    data.output(arg_block->arg(0));
    builder.build_jump(merge_block);
    builder.move_to_end(else_block);
    data.output(arg_block->arg(0));
    builder.build_jump(merge_block);
    builder.move_to_end(merge_block);

    // do the same thing again
    Block* arg_block1 = builder.build_block({Type::Int64});
    builder.build_jump(arg_block1, {arg_block->arg(0)});
    builder.move_to_end(arg_block1);
    data.output(arg_block1->arg(0));
    Value* cond1 = data.input(Type::Bool);
    Block* then_block1 = builder.build_block();
    Block* else_block1 = builder.build_block();
    Block* merge_block1 = builder.build_block();
    builder.build_branch(cond1, then_block1, else_block1);
    builder.move_to_end(then_block1);
    data.output(arg_block1->arg(0));
    builder.build_jump(merge_block1);
    builder.move_to_end(else_block1);
    data.output(arg_block1->arg(0));
    builder.build_jump(merge_block1);
    builder.move_to_end(merge_block1);
    data.output(arg_block1->arg(0));
    builder.build_exit();

    check_simplifycfg(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int64, flags={}, aliasing=0, offset=0
  Store %0, %1, aliasing=0, offset=8
  %3 = Load %0, type=Bool, flags={}, aliasing=0, offset=16
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, %1, aliasing=0, offset=24
  Jump block=b3
b2:
  Store %0, %1, aliasing=0, offset=32
  Jump block=b3
b3:
  Store %0, %1, aliasing=0, offset=40
  %10 = Load %0, type=Bool, flags={}, aliasing=0, offset=48
  Branch %10, true_block=b4, false_block=b5
b4:
  Store %0, %1, aliasing=0, offset=56
  Jump block=b6
b5:
  Store %0, %1, aliasing=0, offset=64
  Jump block=b6
b6:
  Store %0, %1, aliasing=0, offset=72
  Exit
}
)", builder.section());
  });

  suite.diff_test("simplifycfg branch jump thread").run([](Builder& builder, TestData& data) {
    Value* cond = data.input(Type::Bool);
    Block* then_block = builder.build_block();
    Block* else_block = builder.build_block();
    Block* merge_block = builder.build_block();

    // useless branch
    builder.build_branch(cond, then_block, else_block);
    builder.move_to_end(then_block);
    builder.build_jump(merge_block);
    builder.move_to_end(else_block);
    builder.build_jump(merge_block);
    builder.move_to_end(merge_block);
    data.output(cond);
    builder.build_exit();

    check_simplifycfg(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  Store %0, %1, aliasing=0, offset=1
  Exit
}
)", builder.section());
  });

  suite.diff_test("simplifycfg branch jump thread with args").run([](Builder& builder, TestData& data) {
    Value* cond = data.input(Type::Bool);
    Value* val = data.input(Type::Int64);
    Block* then_block = builder.build_block();
    Block* else_block = builder.build_block();
    Block* merge_block = builder.build_block({Type::Int64});

    builder.build_branch(cond, then_block, else_block);
    builder.move_to_end(then_block);
    builder.build_jump(merge_block, {val});
    builder.move_to_end(else_block);
    builder.build_jump(merge_block, {val});
    builder.move_to_end(merge_block);
    data.output(merge_block->arg(0));
    builder.build_exit();

    check_simplifycfg(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int64, flags={}, aliasing=0, offset=8
  Store %0, %2, aliasing=0, offset=16
  Exit
}
)", builder.section());
  });

  suite.diff_test("simplifycfg branch jump thread with const args").run([](Builder& builder, TestData& data) {
    Value* cond = data.input(Type::Bool);
    Block* then_block = builder.build_block();
    Block* else_block = builder.build_block();
    Block* merge_block = builder.build_block({Type::Int64});

    builder.build_branch(cond, then_block, else_block);
    builder.move_to_end(then_block);
    builder.build_jump(merge_block, {builder.build_const(Type::Int64, 1)});
    builder.move_to_end(else_block);
    builder.build_jump(merge_block, {builder.build_const(Type::Int64, 1)});
    builder.move_to_end(merge_block);
    data.output(merge_block->arg(0));
    builder.build_exit();

    check_simplifycfg(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  Store %0, 1:Int64, aliasing=0, offset=8
  Exit
}
)", builder.section());
  });

  suite.diff_test("simplifycfg branch jump thread with args repeat").run([](Builder& builder, TestData& data) {
    Value* cond = data.input(Type::Bool);
    Value* cond2 = data.input(Type::Bool);
    Value* val = data.input(Type::Int64);
    Block* then_block = builder.build_block();
    Block* else_block = builder.build_block();
    Block* then_block2 = builder.build_block();
    Block* else_block2 = builder.build_block();
    Block* merge_block = builder.build_block({Type::Int64});

    builder.build_branch(cond, then_block, else_block);
    builder.move_to_end(then_block);
    builder.build_jump(merge_block, {val});
    builder.move_to_end(else_block);
    builder.build_branch(cond2, then_block2, else_block2);
    builder.move_to_end(then_block2);
    builder.build_jump(merge_block, {val});
    builder.move_to_end(else_block2);
    builder.build_jump(merge_block, {val});
    builder.move_to_end(merge_block);
    data.output(merge_block->arg(0));
    builder.build_exit();

    check_simplifycfg(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Bool, flags={}, aliasing=0, offset=1
  %3 = Load %0, type=Int64, flags={}, aliasing=0, offset=8
  Store %0, %3, aliasing=0, offset=16
  Exit
}
)", builder.section());
  });

  suite.diff_test("simplifycfg jump thread enables block merge").run([](Builder& builder, TestData& data) {
    // entry branches to thread_me or direct. both jump to target({val}).
    // thread_me is a pure passthrough (single jump), so entry gets threaded
    // through it to target directly. After threading, thread_me loses its only
    // predecessor and is removed, leaving target with a single incoming edge
    // from direct. This requires rescheduling.
    Value* cond = data.input(Type::Bool);
    Value* val = data.input(Type::Int64);
    Block* thread_me = builder.build_block();
    Block* direct = builder.build_block();
    Block* target = builder.build_block({Type::Int64});

    builder.build_branch(cond, thread_me, direct);
    builder.move_to_end(thread_me);
    builder.build_jump(target, {val});
    builder.move_to_end(direct);
    builder.build_jump(target, {val});
    builder.move_to_end(target);
    data.output(target->arg(0));
    builder.build_exit();

    check_simplifycfg(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int64, flags={}, aliasing=0, offset=8
  Store %0, %2, aliasing=0, offset=16
  Exit
}
)", builder.section());
  });

  suite.test("block order loop").run([]() {
    Context context;
    Allocator allocator;
    Section* section = new Section(context, allocator);
    Builder builder(section);
    builder.move_to_end(builder.build_block({Type::Ptr}));
    // build exit block first to make it not natural order
    Block* after_block = builder.build_block();

    Block* loop_block = builder.build_block({Type::Int32, Type::Int32});
    Block* next_block = builder.build_block();

    builder.build_jump(loop_block, {builder.build_const(Type::Int32, 0), builder.build_const(Type::Int32, 0)});
    builder.move_to_end(loop_block);

    Value* counter = loop_block->arg(0);
    Value* sum = loop_block->arg(1);

    builder.build_branch(
      builder.build_lt_u(counter, builder.build_const(Type::Int32, 10)),
      next_block,
      after_block
    );

    builder.move_to_end(next_block);
    builder.build_jump(loop_block, {
      builder.build_add(counter, builder.build_const(Type::Int32, 1)),
      builder.build_add(sum, builder.build_const(Type::Int32, 1))
    });

    builder.move_to_end(after_block);
    builder.build_store(builder.entry_arg(0), sum, AliasingGroup(0), 0);
    builder.build_exit();
    builder.section()->set_ordering(BlockOrdering::None);
    check_block_order(R"(section {
b0(%0: Ptr):
  Jump 0:Int32, 0:Int32, block=b1
b1(%2: Int32, %3: Int32):
  %4 = LtU %2, 10:Int32
  Branch %4, true_block=b2, false_block=b3
b2:
  %6 = Add %2, 1:Int32
  %7 = Add %3, 1:Int32
  Jump %6, %7, block=b1
b3:
  Store %0, %3, aliasing=0, offset=0
  Exit
}
)", section);
    assert(section->ordering() == BlockOrdering::Natural);
    // check that verify catches loops if BlockOrdering is Topological
    std::stringstream ss;
    builder.section()->set_ordering(BlockOrdering::Topological);
    unittest_assert (section->verify(ss)); // invalid
    delete section;
  });

  suite.diff_test("block order without loop becomes topological").run([](Builder& builder, TestData& data) {
    Value* cond = data.input(Type::Bool);

    // build exit block first to make it not natural order
    Block* merge_block = builder.build_block();
    Block* then_block = builder.build_block();
    Block* else_block = builder.build_block();

    builder.build_branch(cond, then_block, else_block);
    builder.move_to_end(then_block);
    builder.build_jump(merge_block);
    builder.move_to_end(else_block);
    builder.build_jump(merge_block);
    builder.move_to_end(merge_block);
    data.output(builder.build_const(Type::Int64, 42));
    builder.build_exit();
    builder.section()->set_ordering(BlockOrdering::None);

    check_block_order(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  Branch %1, true_block=b1, false_block=b2
b1:
  Jump block=b3
b2:
  Jump block=b3
b3:
  Store %0, 42:Int64, aliasing=0, offset=8
  Exit
}
)", builder.section());
    assert(builder.section()->ordering() == BlockOrdering::Topological);
  });

  suite.test("cse does not merge distinct allocas").run([]() {
    Context context;
    Allocator allocator;
    Section* section = new Section(context, allocator);
    Builder builder(section);
    builder.move_to_end(builder.build_block({Type::Ptr}));
    Value* size = builder.build_const(Type::Int64, 8);
    Value* a1 = builder.build_alloca(size, 8);
    Value* a2 = builder.build_alloca(size, 8);
    builder.build_store(a1, builder.build_const(Type::Int64, 1), AliasingGroup(0), 0);
    builder.build_store(a2, builder.build_const(Type::Int64, 2), AliasingGroup(0), 0);
    Value* v1 = builder.build_load(a1, Type::Int64, {}, AliasingGroup(0), 0);
    Value* v2 = builder.build_load(a2, Type::Int64, {}, AliasingGroup(0), 0);
    builder.build_store(builder.entry_arg(0), builder.build_add(v1, v2), AliasingGroup(0), 0);
    builder.build_exit();
    section->set_ordering(BlockOrdering::Dominator);
    check_cse(R"(section {
b0(%0: Ptr):
  %1 = Alloca 8:Int64, align=8
  %2 = Alloca 8:Int64, align=8
  Store %1, 1:Int64, aliasing=0, offset=0
  Store %2, 2:Int64, aliasing=0, offset=0
  %5 = Load %1, type=Int64, flags={}, aliasing=0, offset=0
  %6 = Load %2, type=Int64, flags={}, aliasing=0, offset=0
  %7 = Add %5, %6
  Store %0, %7, aliasing=0, offset=0
  Exit
}
)", section);
    delete section;
  });

  suite.test("cse merges across dominator chain").run([]() {
    Context context;
    Allocator allocator;
    Section* section = new Section(context, allocator);
    Builder builder(section);
    Block* b0 = builder.build_block({Type::Int64, Type::Int64, Type::Ptr});
    builder.move_to_end(b0);
    Value* a = b0->arg(0);
    Value* b = b0->arg(1);
    Value* ptr = b0->arg(2);
    builder.build_add(a, b);
    Block* b1 = builder.build_block();
    builder.build_jump(b1);
    builder.move_to_end(b1);
    Block* b2 = builder.build_block();
    builder.build_jump(b2);
    builder.move_to_end(b2);
    Value* sum2 = builder.build_add(b, a);
    builder.build_store(ptr, sum2, AliasingGroup(0), 0);
    builder.build_exit();
    section->set_ordering(BlockOrdering::Dominator);
    check_cse(R"(section {
b0(%0: Int64, %1: Int64, %2: Ptr):
  %3 = Add %0, %1
  Jump block=b1
b1:
  Jump block=b2
b2:
  Store %2, %3, aliasing=0, offset=0
  Exit
}
)", section);
    delete section;
  });

  suite.test("cse does not merge across sibling branches").run([]() {
    Context context;
    Allocator allocator;
    Section* section = new Section(context, allocator);
    Builder builder(section);
    Block* b0 = builder.build_block({Type::Int64, Type::Int64, Type::Bool, Type::Ptr});
    builder.move_to_end(b0);
    Value* a = b0->arg(0);
    Value* b = b0->arg(1);
    Value* cond = b0->arg(2);
    Value* ptr = b0->arg(3);
    Block* b1 = builder.build_block();
    Block* b2 = builder.build_block();
    Block* b3 = builder.build_block();
    builder.build_branch(cond, b1, b2);

    builder.move_to_end(b1);
    Value* sum1 = builder.build_add(a, b);
    builder.build_store(ptr, sum1, AliasingGroup(0), 0);
    builder.build_jump(b3);

    builder.move_to_end(b2);
    Value* sum2 = builder.build_add(b, a);
    builder.build_store(ptr, sum2, AliasingGroup(0), 8);
    builder.build_jump(b3);

    builder.move_to_end(b3);
    builder.build_exit();
    section->set_ordering(BlockOrdering::Dominator);
    check_cse(R"(section {
b0(%0: Int64, %1: Int64, %2: Bool, %3: Ptr):
  Branch %2, true_block=b1, false_block=b2
b1:
  %5 = Add %0, %1
  Store %3, %5, aliasing=0, offset=0
  Jump block=b3
b2:
  %8 = Add %1, %0
  Store %3, %8, aliasing=0, offset=8
  Jump block=b3
b3:
  Exit
}
)", section);
    delete section;
  });

  suite.test("cse does not merge loads across block boundary").run([]() {
    Context context;
    Allocator allocator;
    Section* section = new Section(context, allocator);
    Builder builder(section);
    Block* b0 = builder.build_block({Type::Bool, Type::Ptr});
    builder.move_to_end(b0);
    Value* cond = b0->arg(0);
    Value* out = b0->arg(1);
    Value* size = builder.build_const(Type::Int64, 8);
    Value* alloca = builder.build_alloca(size, 8);
    builder.build_store(alloca, builder.build_const(Type::Int64, 1), AliasingGroup(0), 0);
    Value* l0 = builder.build_load(alloca, Type::Int64, {}, AliasingGroup(0), 0);

    Block* b1 = builder.build_block();
    Block* b2 = builder.build_block();
    Block* b3 = builder.build_block();
    builder.build_branch(cond, b1, b2);

    builder.move_to_end(b1);
    builder.build_store(alloca, builder.build_const(Type::Int64, 2), AliasingGroup(0), 0);
    Value* l1 = builder.build_load(alloca, Type::Int64, {}, AliasingGroup(0), 0);
    builder.build_store(out, l1, AliasingGroup(0), 0);
    builder.build_jump(b3);

    builder.move_to_end(b2);
    Value* l2 = builder.build_load(alloca, Type::Int64, {}, AliasingGroup(0), 0);
    builder.build_store(out, l2, AliasingGroup(0), 0);
    builder.build_jump(b3);

    builder.move_to_end(b3);
    builder.build_exit();
    (void) l0;
    section->set_ordering(BlockOrdering::Dominator);
    check_cse(R"(section {
b0(%0: Bool, %1: Ptr):
  %2 = Alloca 8:Int64, align=8
  Store %2, 1:Int64, aliasing=0, offset=0
  %4 = Load %2, type=Int64, flags={}, aliasing=0, offset=0
  Branch %0, true_block=b1, false_block=b2
b1:
  Store %2, 2:Int64, aliasing=0, offset=0
  %7 = Load %2, type=Int64, flags={}, aliasing=0, offset=0
  Store %1, %7, aliasing=0, offset=0
  Jump block=b3
b2:
  %10 = Load %2, type=Int64, flags={}, aliasing=0, offset=0
  Store %1, %10, aliasing=0, offset=0
  Jump block=b3
b3:
  Exit
}
)", section);
    delete section;
  });

  suite.test("cse does not merge loads across block boundary after call").run([]() {
    Context context;
    Allocator allocator;
    Section* section = new Section(context, allocator);
    Builder builder(section);
    Block* b0 = builder.build_block({Type::Bool, Type::Ptr});
    builder.move_to_end(b0);
    Value* cond = b0->arg(0);
    Value* out = b0->arg(1);
    Value* size = builder.build_const(Type::Int64, 8);
    Value* alloca = builder.build_alloca(size, 8);
    builder.build_store(alloca, builder.build_const(Type::Int64, 1), AliasingGroup(0), 0);
    Value* l0 = builder.build_load(alloca, Type::Int64, {}, AliasingGroup(0), 0);
    Value* callee = section->context().build_symbol(Type::Ptr, "cse_call_invalidation_target");

    Block* b1 = builder.build_block();
    Block* b2 = builder.build_block();
    Block* b3 = builder.build_block();
    builder.build_branch(cond, b1, b2);

    builder.move_to_end(b1);
    builder.build_call(callee, Type::Void, std::vector<Value*>{}, CallConv::Default);
    Value* l1 = builder.build_load(alloca, Type::Int64, {}, AliasingGroup(0), 0);
    builder.build_store(out, l1, AliasingGroup(0), 0);
    builder.build_jump(b3);

    builder.move_to_end(b2);
    Value* l2 = builder.build_load(alloca, Type::Int64, {}, AliasingGroup(0), 0);
    builder.build_store(out, l2, AliasingGroup(0), 0);
    builder.build_jump(b3);

    builder.move_to_end(b3);
    builder.build_exit();
    (void) l0;
    section->set_ordering(BlockOrdering::Dominator);
    check_cse(R"(section {
b0(%0: Bool, %1: Ptr):
  %2 = Alloca 8:Int64, align=8
  Store %2, 1:Int64, aliasing=0, offset=0
  %4 = Load %2, type=Int64, flags={}, aliasing=0, offset=0
  Branch %0, true_block=b1, false_block=b2
b1:
  Call @cse_call_invalidation_target:Ptr, type=Void, call_conv=Default, flags={}
  %7 = Load %2, type=Int64, flags={}, aliasing=0, offset=0
  Store %1, %7, aliasing=0, offset=0
  Jump block=b3
b2:
  %10 = Load %2, type=Int64, flags={}, aliasing=0, offset=0
  Store %1, %10, aliasing=0, offset=0
  Jump block=b3
b3:
  Exit
}
)", section);
    delete section;
  });

  suite.test("cse does not merge a load reused unmodified across a block boundary").run([]() {
    Context context;
    Allocator allocator;
    Section* section = new Section(context, allocator);
    Builder builder(section);
    Block* b0 = builder.build_block({Type::Ptr});
    builder.move_to_end(b0);
    Value* ptr = b0->arg(0);
    Value* l0 = builder.build_load(ptr, Type::Int64, {}, AliasingGroup(0), 0);

    Block* b1 = builder.build_block();
    builder.build_jump(b1);

    builder.move_to_end(b1);
    Value* l1 = builder.build_load(ptr, Type::Int64, {}, AliasingGroup(0), 0);
    builder.build_store(ptr, l1, AliasingGroup(0), 8);
    builder.build_exit();
    (void) l0;
    section->set_ordering(BlockOrdering::Dominator);
    check_cse(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int64, flags={}, aliasing=0, offset=0
  Jump block=b1
b1:
  %3 = Load %0, type=Int64, flags={}, aliasing=0, offset=0
  Store %0, %3, aliasing=0, offset=8
  Exit
}
)", section);
    delete section;
  });

  suite.test("trace_builder_shared_exits").run(trace_builder_shared_exits);
  suite.test("trace_builder_failure_store_preserves_forwarding").run(trace_builder_failure_store_preserves_forwarding);
  suite.test("trace_builder_failure_load_bypasses_forwarding").run(trace_builder_failure_load_bypasses_forwarding);
  suite.test("trace_builder_false_guard").run(trace_builder_false_guard);

  suite.test("trace_builder_guard_excludes_failure_from_chain").run([]() {
    for (bool expected : {false, true}) {
      Context context;
      Allocator allocator;
      Section section(context, allocator);
      TraceBuilder builder(&section);
      Chain chain;
      builder.set_chain(&chain);
      Block* entry = builder.build_block(std::vector<Type>{Type::Bool});
      builder.move_to_end(entry);
      Value* cond = expected ? entry->arg(0) : builder.build_xor(entry->arg(0), builder.build_const(Type::Bool, 1));
      builder.build_guard_begin(cond);
      builder.build_guard_end();
      Block* success = builder.block();
      builder.build_exit();

      unittest_assert(!section.verify(std::cout));
      unittest_assert(chain.size() == 2);
      unittest_assert(chain.front() == entry);
      unittest_assert(chain.back() == success);
    }
  });

  return suite.finish();
}
