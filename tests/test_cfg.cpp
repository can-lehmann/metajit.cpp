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

#include "../../unittest.cpp/unittest.hpp"

using namespace metajit;
using namespace metajit::test;

int main() {
  metajit::LLVMCodeGen::initilize_llvm_jit();

  DiffTestSuite suite("tests/output/test_cfg");

  suite.diff_test("branch").run([](Builder& builder, TestData& data) {
    Block* a = builder.build_block();
    Block* b = builder.build_block();
    Block* cont = builder.build_block({Type::Int64});

    Value* cond = data.input(Type::Bool);
    Value* value_a = data.input(Type::Int64);
    Value* value_b = data.input(Type::Int64);

    builder.build_branch(cond, a, b);

    builder.move_to_end(a);
    builder.build_jump(cont, {value_a});

    builder.move_to_end(b);
    builder.build_jump(cont, {value_b});

    builder.move_to_end(cont);
    data.output(cont->arg(0));

  });

  suite.diff_test("sum_to").run([](Builder& builder, TestData& data) {
    Block* loop_header = builder.build_block({Type::Int64, Type::Int64}); // (i, sum)
    Block* loop_body = builder.build_block();
    Block* loop_end = builder.build_block();

    Value* n = data.input(RandomRange(Type::Int64, 1, 100));

    builder.build_jump(loop_header, {
      builder.build_const(Type::Int64, 0),
      builder.build_const(Type::Int64, 0)
    });

    builder.move_to_end(loop_header);
    builder.build_branch(
      builder.build_lt_u(
        loop_header->arg(0),
        n
      ),
      loop_body,
      loop_end
    );

    builder.move_to_end(loop_body);
    builder.build_jump(loop_header, {
      builder.build_add(
        loop_header->arg(0),
        builder.build_const(Type::Int64, 1)
      ),
      builder.build_add(
        loop_header->arg(1),
        loop_header->arg(0)
      )
    });

    builder.move_to_end(loop_end);
    data.output(loop_header->arg(1));
  });

  suite.diff_test("fib").run([](Builder& builder, TestData& data) {
    Block* loop_header = builder.build_block({Type::Int64, Type::Int64, Type::Int64}); // (i, a, b)
    Block* loop_body = builder.build_block();
    Block* loop_end = builder.build_block();

    Value* n = data.input(RandomRange(Type::Int64, 1, 100));

    builder.build_jump(loop_header, {
      builder.build_const(Type::Int64, 0),
      builder.build_const(Type::Int64, 0),
      builder.build_const(Type::Int64, 1)
    });

    builder.move_to_end(loop_header);
    builder.build_branch(
      builder.build_lt_u(
        loop_header->arg(0),
        n
      ),
      loop_body,
      loop_end
    );

    builder.move_to_end(loop_body);
    builder.build_jump(loop_header, {
      builder.build_add(
        loop_header->arg(0),
        builder.build_const(Type::Int64, 1)
      ),
      loop_header->arg(2),
      builder.build_add(
        loop_header->arg(1),
        loop_header->arg(2)
      )
    });

    builder.move_to_end(loop_end);
    data.output(loop_header->arg(1));
  });

  suite.diff_test("swap_loop").run([](Builder& builder, TestData& data) {
    Block* loop_header = builder.build_block({Type::Bool, Type::Int64, Type::Int64}); // (cond, a, b)
    Block* loop_body = builder.build_block();
    Block* loop_end = builder.build_block();

    Value* a = data.input(RandomRange(Type::Int64));
    Value* b = data.input(RandomRange(Type::Int64));
    Value* cond = data.input(Type::Bool);

    builder.build_jump(loop_header, {cond, a, b});

    builder.move_to_end(loop_header);
    builder.build_branch(
      loop_header->arg(0),
      loop_body,
      loop_end
    );

    builder.move_to_end(loop_body);
    builder.build_jump(loop_header, {
      builder.build_const(Type::Bool, false),
      loop_header->arg(2),
      loop_header->arg(1)
    });

    builder.move_to_end(loop_end);
    data.output(loop_header->arg(1));
    data.output(loop_header->arg(2));
  });

  suite.test("dominator_order_verify").run([]() {
    Context context;
    Allocator allocator;
    Section* section = new Section(context, allocator);
    Builder builder(section);

    Block* entry = builder.build_block();
    Block* a = builder.build_block();
    Block* b = builder.build_block();

    builder.move_to_end(entry);
    builder.build_jump(a);

    builder.move_to_end(a);
    builder.build_jump(b);

    builder.move_to_end(b);
    builder.build_exit();

    section->set_ordering(BlockOrdering::Dominator);

    // Natural order [entry, a, b] should pass
    {
      std::stringstream ss;
      unittest_assert(!section->verify(ss));
    }

    // Swapped order [entry, b, a] should fail (a dominates b, so a must come before b)
    section->remove(a);
    section->add(a);

    {
      std::stringstream ss;
      unittest_assert(section->verify(ss));
      unittest_assert(ss.str() == "Block b1 appears before its immediate dominator b2\n");
    }

    delete section;
    });
  return suite.finish();
}
