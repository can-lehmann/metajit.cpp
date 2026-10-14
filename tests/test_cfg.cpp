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

int main(int argc, char** argv) {
  metajit::LLVMCodeGen::initilize_llvm_jit();

  DiffTestSuite suite("tests/output/test_cfg", argc, argv);

  for (Type type : {Type::Float32, Type::Float64, Type::Int32, Type::Int64}) {
    suite.test(std::string("entry_argument_") + to_string(type)).run([type]() {
      for (auto mode : {X86CodeGen::Mode::JIT, X86CodeGen::Mode::AOT}) {
        Context context;
        Allocator allocator;
        Section* section = new Section(context, allocator);
        Builder builder(section);
        Block* entry = builder.build_block({type, Type::Ptr});
        builder.move_to_end(entry);
        builder.build_store(entry->arg(1), entry->arg(0), AliasingGroup(0), 0);
        builder.build_exit();
        section->autoname();
        section->set_ordering(BlockOrdering::Natural);

        uint64_t bits = 0x123456789abcdef0;
        uint64_t result = 0;
        if (is_float(type)) {
          X86CodeGen codegen(section, {Reg::X86_R12(), Reg::X86_R13()}, mode);
          using Func = void(* [[clang::preserve_none]])(uint64_t, uint64_t*);
          ((Func) codegen.deploy())(bits, &result);
        } else {
          X86CodeGen codegen(section, {Reg::X86_XMM(0), Reg::X86_R12()}, mode);
          using Func = void(* [[clang::preserve_none]])(double, uint64_t*);
          ((Func) codegen.deploy())(bit_cast<double>(bits), &result);
        }
        unittest_assert(result == (bits & type_mask(type)));
        delete section;
      }
    });
  }

  suite.diff_test("entry_argument_spilled_before_first_use").aot(false).run([](Builder& builder, TestData& data) {
    std::vector<Value*> values;
    for (size_t index = 0; index < 32; index++) {
      values.push_back(builder.build_add(
        builder.build_const(Type::Int64, index + 1),
        builder.build_const(Type::Int64, index + 2)
      ));
    }
    Value* input = data.input(Type::Int64);
    for (Value* value : values) {
      data.output(value);
    }
    data.output(input);
  });

  for (Type type : {Type::Float32, Type::Float64}) {
    suite.diff_test(std::string("float_block_argument_") + to_string(type)).run([type](Builder& builder, TestData& data) {
      Block* a = builder.build_block();
      Block* b = builder.build_block();
      Block* cont = builder.build_block({type});
      Value* cond = data.input(Type::Bool);
      Value* value_a = data.input(type);
      Value* value_b = data.input(type);
      builder.build_branch(cond, a, b);
      builder.move_to_end(a);
      builder.build_jump(cont, {value_a});
      builder.move_to_end(b);
      builder.build_jump(cont, {value_b});
      builder.move_to_end(cont);
      data.output(cont->arg(0));
    });

    suite.diff_test(std::string("float_swap_loop_") + to_string(type)).run([type](Builder& builder, TestData& data) {
      Block* header = builder.build_block({Type::Bool, type, type});
      Block* body = builder.build_block();
      Block* end = builder.build_block();
      Value* a = data.input(type);
      Value* b = data.input(type);
      Value* cond = data.input(Type::Bool);
      builder.build_jump(header, {cond, a, b});
      builder.move_to_end(header);
      builder.build_branch(header->arg(0), body, end);
      builder.move_to_end(body);
      builder.build_jump(header, {
        builder.build_const(Type::Bool, false),
        header->arg(2), header->arg(1)
      });
      builder.move_to_end(end);
      data.output(header->arg(1));
      data.output(header->arg(2));
    });
  }

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

  suite.diff_test("critical_edge").aot(false).run([](Builder& builder, TestData& data) {
    Block* a = builder.build_block();
    Block* b = builder.build_block();
    Block* other = builder.build_block();
    Block* merge = builder.build_block();

    Value* first_cond = data.input(Type::Bool);
    Value* second_cond = data.input(Type::Bool);
    std::vector<Value*> values;
    for (size_t index = 0; index < 24; index++) {
      values.push_back(data.input(Type::Int64));
    }
    builder.build_branch(first_cond, a, b);

    builder.move_to_end(a);
    builder.build_jump(merge);

    builder.move_to_end(b);
    for (Value* value : values) {
      data.output(builder.build_add(value, builder.build_const(Type::Int64, 1)));
    }
    builder.build_branch(second_cond, merge, other);

    builder.move_to_end(other);
    data.output(builder.build_const(Type::Int64, 42));
    builder.build_jump(merge);

    builder.move_to_end(merge);
    for (Value* value : values) {
      data.output(value);
    }
  });

  suite.diff_test("critical_backedge").aot(false).run([](Builder& builder, TestData& data) {
    Block* header = builder.build_block();
    Block* body = builder.build_block();
    Block* end = builder.build_block();

    Value* count = data.input(RandomRange(Type::Int64, 1, 8));
    Value* counter = builder.build_alloca(builder.build_const(Type::Int64, 8), 8);
    builder.build_store(counter, count, AliasingGroup(0), 0);
    std::vector<Value*> values;
    for (size_t index = 0; index < 24; index++) {
      values.push_back(data.input(Type::Int64));
    }
    builder.build_jump(header);

    builder.move_to_end(header);
    Value* current = builder.build_load(counter, Type::Int64, LoadFlags::None, AliasingGroup(0), 0);
    builder.build_jump(body);

    builder.move_to_end(body);
    Value* next = builder.build_sub(current, builder.build_const(Type::Int64, 1));
    builder.build_store(counter, next, AliasingGroup(0), 0);
    for (Value* value : values) {
      data.output(builder.build_add(value, next));
    }
    builder.build_branch(builder.build_lt_u(builder.build_const(Type::Int64, 0), next), header, end);

    builder.move_to_end(end);
    for (Value* value : values) {
      data.output(value);
    }
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

  suite.diff_test("multi_arg_merge_failure").run([](Builder& builder, TestData& data) {
    // Fill registers to force specific assignments
    Block* header = builder.build_block({
      Type::Int64, Type::Int64, Type::Int64, Type::Int64,
      Type::Int64, Type::Int64, Type::Int64, Type::Int64,
      Type::Int64, Type::Int64, Type::Int64
    });
    Block* loop_body = builder.build_block();
    Block* exit = builder.build_block();

    std::vector<Value*> initial_args;
    for (size_t j = 0; j < 10; ++j) initial_args.push_back(data.input(Type::Int64));
    initial_args.push_back(builder.build_const(Type::Int64, 0)); // i

    builder.build_jump(header, initial_args);

    builder.move_to_end(header);
    Value* x = header->arg(0);
    Value* i = header->arg(10);

    Value* s = x;
    for (size_t j = 0; j < 10; ++j) {
      s = builder.build_add(s, header->arg(j));
    }

    Value* cond = builder.build_lt_s(i, builder.build_const(Type::Int64, 3));
    builder.build_branch(cond, loop_body, exit);

    builder.move_to_end(loop_body);
    std::vector<Value*> back_args;
    // Pass x to both slot 0 and slot 1
    back_args.push_back(x);
    back_args.push_back(x);
    for (size_t j = 2; j < 10; ++j) back_args.push_back(header->arg(j));
    back_args.push_back(builder.build_add(i, builder.build_const(Type::Int64, 1)));

    builder.build_jump(header, back_args);

    builder.move_to_end(exit);
    data.output(s);
  });

  suite.test("substitute blocks").run([]() {
    for (bool dense : {false, true}) {
      Context context;
      Allocator allocator;
      Section section(context, allocator);
      Builder builder(&section);
      Block* entry = builder.build_block({Type::Bool});
      Block* left = builder.build_block();
      Block* right = builder.build_block();
      Block* merge = builder.build_block({Type::Int8});
      Block* target = builder.build_block({Type::Int8});
      builder.move_to_end(entry);
      BranchInst* branch = builder.build_branch(entry->arg(0), left, right);
      builder.move_to_end(left);
      Value* value = builder.build_const(Type::Int8, 17);
      JumpInst* jump = builder.build_jump(merge, {value});
      builder.move_to_end(right);
      Inst* exit = builder.build_exit();
      builder.move_to_end(merge);
      JumpInst* unchanged = builder.build_jump(target, {merge->arg(0)});
      builder.move_to_end(target);
      Inst* add = builder.build_add(target->arg(0), value);
      builder.build_exit();
      section.autoname();

      auto check = [&](auto& substs) {
        branch->substitute_blocks(substs);
        jump->substitute_blocks(substs);
        unchanged->substitute_blocks(substs);
        exit->substitute_blocks(substs);
        add->substitute_blocks(substs);
        unittest_assert(branch->true_block() == right && branch->false_block() == left);
        unittest_assert(branch->arg(0) == entry->arg(0));
        unittest_assert(jump->block() == target && jump->arg(0) == value);
        unittest_assert(unchanged->block() == target && unchanged->arg(0) == merge->arg(0));
        unittest_assert(add->arg(0) == target->arg(0) && add->arg(1) == value);
        branch->set_true_block(left);
        branch->set_false_block(left);
        branch->substitute_blocks(substs);
        unittest_assert(branch->true_block() == right && branch->false_block() == right);
      };
      if (dense) {
        BlockMap<Block*> substs(&section);
        substs[left] = right;
        substs[right] = left;
        substs[merge] = target;
        check(substs);
      } else {
        const std::map<Block*, Block*> substs = {{left, right}, {right, left}, {merge, target}};
        check(substs);
      }
    }
  });

  suite.test("branch region exits").run([]() {
    for (int mode = 0; mode < 3; mode++) {
      Context context;
      Allocator allocator;
      Section section(context, allocator);
      Builder builder(&section);
      Block* entry = builder.build_block({Type::Bool});
      Block* left = builder.build_block();
      Block* right = builder.build_block();
      Block* merge = builder.build_block();
      builder.move_to_end(entry);
      builder.build_branch(entry->arg(0), left, right);
      builder.move_to_end(left);
      builder.build_jump(merge);
      builder.move_to_end(right);
      if (mode == 0) {
        builder.build_jump(merge);
      } else if (mode == 1) {
        builder.build_exit();
      } else {
        builder.build_jump(right);
      }
      builder.move_to_end(merge);
      builder.build_exit();
      OrderBlocks::run(&section, BlockOrdering::Natural);
      DominatorTree dom(&section);
      Incoming incoming(&section);
      PostDominatorTree postdom(incoming);
      if (mode == 0) {
        unittest_assert(postdom.common_dominator(left, right) == merge);
        unittest_assert(BranchRegion::find_exit(entry, dom, postdom) == merge);
      } else if (mode == 1) {
        unittest_assert(postdom.common_dominator(left, right) == postdom.virtual_exit());
        unittest_assert(BranchRegion::find_exit(entry, dom, postdom) == nullptr);
      } else {
        unittest_assert(postdom.common_dominator(left, right) == nullptr);
        unittest_assert(BranchRegion::find_exit(entry, dom, postdom) == nullptr);
      }
      std::stringstream dot;
      postdom.write_dot(dot);
      unittest_assert(!dot.str().empty());
    }
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
