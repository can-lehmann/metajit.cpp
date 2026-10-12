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

#include "diff.hpp"

using namespace metajit;
using namespace metajit::test;

void promoted_arithmetic_guard(Builder& builder, TraceTestData& data) {
  Value* value = data.input(RandomRange(Type::Int32));
  Value* promoted = builder.build_promote(value);
  data.output(builder.build_add(promoted, builder.build_const(Type::Int32, 4)));
}

void folded_branch_guard(Builder& builder, TraceTestData& data) {
  Value* x = data.input(RandomRange(Type::Int32));
  Value* cond = builder.build_eq(x, x);
  Block* true_block = builder.build_block();
  Block* false_block = builder.build_block();
  builder.build_branch(cond, true_block, false_block);
  builder.move_to_end(true_block);
  data.output(x);
  builder.build_exit();
  builder.move_to_end(false_block);
  data.output(builder.build_const(Type::Int32, 123));
  builder.build_exit();
}

void shared_promotion_exits(Builder& builder, TraceTestData& data) {
  Value* x = builder.build_promote(data.input(RandomRange(Type::Int32, 0, 3)));
  Value* y = data.input(RandomRange(Type::Int32, 0, 3));
  y = builder.build_promote(builder.build_add(y, builder.build_const(Type::Int32, 1)));
  Value* z = builder.build_promote(data.input(RandomRange(Type::Int32, 0, 3)));
  data.output(builder.build_add(builder.build_add(x, y), z));
}

void shared_branch_exit(Builder& builder, TraceTestData& data) {
  Value* cond = data.input(RandomRange(Type::Bool));
  Value* x = data.input(RandomRange(Type::Int32, 0, 3));
  data.keep(x);
  Block* true_block = builder.build_block();
  Block* false_block = builder.build_block();
  builder.build_branch(cond, true_block, false_block);
  builder.move_to_end(true_block);
  data.output(builder.build_add(builder.build_promote(x), builder.build_const(Type::Int32, 10)));
  builder.build_exit();
  builder.move_to_end(false_block);
  data.output(builder.build_add(builder.build_promote(x), builder.build_const(Type::Int32, 20)));
  builder.build_exit();
}

void shared_promotion_branch_exit(Builder& builder, TraceTestData& data) {
  Value* x = builder.build_promote(data.input(RandomRange(Type::Int32, 0, 3)));
  Value* cond = data.input(RandomRange(Type::Bool));
  Block* true_block = builder.build_block();
  Block* false_block = builder.build_block();
  builder.build_branch(cond, true_block, false_block);
  builder.move_to_end(true_block);
  data.output(builder.build_add(x, builder.build_const(Type::Int32, 10)));
  builder.build_exit();
  builder.move_to_end(false_block);
  data.output(builder.build_add(x, builder.build_const(Type::Int32, 20)));
  builder.build_exit();
}

void store_separates_exits(Builder& builder, TraceTestData& data) {
  Value* x = builder.build_promote(data.input(RandomRange(Type::Int32, 0, 3)));
  size_t offset = data.alloc_output(Type::Int32);
  Value* ptr = builder.entry_arg(0);
  Value* old = builder.build_load(ptr, Type::Int32, LoadFlags::None, AliasingGroup(0), offset);
  Value* increment = builder.build_add(x, builder.build_const(Type::Int32, 1));
  builder.build_store(ptr, builder.build_add(old, increment), AliasingGroup(0), offset);
  Value* y = builder.build_promote(data.input(RandomRange(Type::Int32, 0, 3)));
  data.output(y);
}

int main(int argc, char** argv) {
  LLVMCodeGen::initilize_llvm_jit();

  GenExtTestSuite suite("tests/output/test_genext", argc, argv);

  for (bool record_replay : {false, true}) {
    suite.set_record_replay(record_replay);

    suite.gen_ext_test("promoted_arithmetic_guard").run(promoted_arithmetic_guard);
    suite.gen_ext_test("folded_branch_guard").guards(0, 0).run(folded_branch_guard);
    suite.gen_ext_test("shared_promotion_exits").guards(3, 1).run(shared_promotion_exits);
    suite.gen_ext_test("shared_branch_exit").guards(2, 1).run(shared_branch_exit);
    suite.gen_ext_test("shared_promotion_branch_exit").guards(2, 1).run(shared_promotion_branch_exit);
    suite.gen_ext_test("store_separates_exits").guards(2, 2).run(store_separates_exits);

    suite.gen_ext_test("add_promoted").run([](Builder& builder, TraceTestData& data) {
      Value* x = data.static_input(RandomRange(Type::Int32));  // promoted
      Value* y = data.input(RandomRange(Type::Int32));         // dynamic
      Value* result = builder.build_add(x, y);
      data.output(result);
    });

    suite.gen_ext_test("mul_add_promoted").run([](Builder& builder, TraceTestData& data) {
      Value* a = data.static_input(RandomRange(Type::Int32));  // promoted
      Value* b = data.static_input(RandomRange(Type::Int32));  // promoted
      Value* c = data.input(RandomRange(Type::Int32));         // dynamic
      Value* mul = builder.build_mul(a, b);
      Value* result = builder.build_add(mul, c);
      data.output(result);
    });

    suite.gen_ext_test("add_dynamic").run([](Builder& builder, TraceTestData& data) {
      Value* x = data.input(RandomRange(Type::Int32));
      Value* y = data.input(RandomRange(Type::Int32));
      Value* result = builder.build_add(x, y);
      data.output(result);
    });

    suite.gen_ext_test("bug_poison").samples(4, 16).run([](Builder& builder, TraceTestData& data) {
      Value* input = data.input(RandomRange(Type::Int64));
      // quadratic reciprocity
      Value* prime = builder.build_const(Type::Int64, 1000000007ULL);
      Value* residue = builder.build_mod_u(input, prime);
      Value* square = builder.build_mod_u(builder.build_mul(residue, residue), prime);
      Value* equals_five = builder.build_eq(square, builder.build_const(Type::Int64, 5));
      Value* cond = builder.build_eq(equals_five, builder.build_const(Type::Bool, 0));
      Value* shl2 = builder.build_shl(builder.build_const(Type::Int32, 0), builder.build_const(Type::Int32, 32));
      Value* final_select = builder.build_select(cond, builder.build_const(Type::Int32, 0), shl2);
      data.output(final_select);
    });

    suite.gen_ext_test("bug_promote_ptr").run([](Builder& builder, TraceTestData& data) {
      // This test should trigger the bug where Promote on a pointer type
      // generates invalid LLVM IR (zext ptr instead of ptrtoint)
      Value* ptr = data.static_input(RandomRange(Type::Ptr));  // promoted pointer
      Value* offset = data.input(RandomRange(Type::Int64));    // dynamic offset
      Value* result = builder.build_add_ptr(ptr, offset);
      data.output(result);
    });

    suite.gen_ext_test("promote_guard_abort").run([](Builder& builder, TraceTestData& data) {
      Value* x = data.input(RandomRange(Type::Int32));
      x = builder.build_promote(x);
      data.output(x);
    });

    suite.gen_ext_test("branch_guard").run([](Builder& builder, TraceTestData& data) {
      Value* x = data.input(RandomRange(Type::Bool));

      Block* true_block = builder.build_block();
      Block* false_block = builder.build_block();

      builder.build_branch(x, true_block, false_block);

      builder.move_to_end(true_block);
      data.output(builder.build_const(Type::Int32, 123));
      builder.build_exit();

      builder.move_to_end(false_block);
      data.output(builder.build_const(Type::Int32, 456));
      builder.build_exit();
    });

    suite.gen_ext_test("static_branch_guard").run([](Builder& builder, TraceTestData& data) {
      Value* cond = data.static_input(RandomRange(Type::Bool));

      Block* true_block = builder.build_block();
      Block* false_block = builder.build_block();

      builder.build_branch(cond, true_block, false_block);

      builder.move_to_end(true_block);
      data.output(builder.build_const(Type::Int32, 123));
      builder.build_exit();

      builder.move_to_end(false_block);
      data.output(builder.build_const(Type::Int32, 456));
      builder.build_exit();
    });

    suite.gen_ext_test("static_promote_guard").run([](Builder& builder, TraceTestData& data) {
      Value* x = data.static_input(RandomRange(Type::Int32));

      Block* cont_block = builder.build_block({Type::Int32});
      builder.build_jump(cont_block, {x});

      builder.move_to_end(cont_block);
      Value* promoted = builder.build_promote(cont_block->arg(0));
      data.output(promoted);
    });

    suite.gen_ext_test("bug_reentry_captures_dead_value").run([](Builder& builder, TraceTestData& data) {
      Value* x = data.input(RandomRange(Type::Int32));
      data.keep(builder.build_const(Type::Int32, 0));
      Value* cond = data.static_input(RandomRange(Type::Bool));
      data.keep(cond);
      Value* result = builder.build_select(cond, builder.build_const(Type::Int32, 0), x);
      data.output(result);
    });

    suite.gen_ext_test("bug_reentry_captures_alloca").run([](Builder& builder, TraceTestData& data) {
      Value* x = data.input(RandomRange(Type::Int32));
      Value* ptr = builder.build_alloca(Type::Int32);
      builder.build_store(ptr, x, AliasingGroup(-1), 0);
      Value* cond = data.static_input(RandomRange(Type::Bool));
      data.keep(cond);
      Value* loaded = builder.build_load(ptr, Type::Int32, LoadFlags::None, AliasingGroup(-1), 0);
      data.output(loaded);
    });

    suite.gen_ext_test("promote_twice").run([](Builder& builder, TraceTestData& data) {
      Value* x = data.input(RandomRange(Type::Int32, 0, 3));
      x = builder.build_promote(x);
      data.output(x);
      Value* y = data.input(RandomRange(Type::Int32, 0, 3));
      y = builder.build_promote(y);
      data.output(y);
    });
    
  }

  return suite.finish();
}
