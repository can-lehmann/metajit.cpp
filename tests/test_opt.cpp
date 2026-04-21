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

int main() {
  metajit::LLVMCodeGen::initilize_llvm_jit();

  DiffTestSuite suite("tests/output/test_opt");

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
  %2 = ResizeX %1, type=Int8
  %3 = And %1, 255:Int64
  Store %0, %3, aliasing=0, offset=8
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
  %2 = ShrU %1, 1:Int64
  %3 = And %1, 6:Int64
  Store %0, %3, aliasing=0, offset=8
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
  %2 = ShrU %1, 10:Int64
  %3 = ShrU %1, 9:Int64
  %4 = And %3, 2:Int64
  Store %0, %4, aliasing=0, offset=8
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
  %2 = ShrU %1, 1:Int64
  %3 = Shl %1, 2:Int64
  %4 = And %3, 8:Int64
  Store %0, %4, aliasing=0, offset=8
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
  %2 = Shl %1, 1:Int64
  %3 = Shl %1, 4:Int64
  %4 = And %3, 2040:Int64
  Store %0, %4, aliasing=0, offset=8
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
  %3 = Or %2, %1
  %4 = And %2, 4294902015:Int64
  Store %0, %4, aliasing=0, offset=16
}
)", builder.section());
  });

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
  %3 = ResizeX %1, type=Int64
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
  %4 = Shl %2, 14:Int16
  %5 = Or %3, %1
  %6 = Or %5, %4
  %7 = And %1, 4095:Int16
  Store %0, %7, aliasing=0, offset=6
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

  // tests for SimplifyCFG
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
  Branch %1, true_block=b1, false_block=b2
b1:
  Jump block=b2
b2:
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
  Branch %1, true_block=b1, false_block=b2
b1:
  Jump block=b2
b2:
  %5 = Load %0, type=Bool, flags={}, aliasing=0, offset=16
  Branch %5, true_block=b3, false_block=b4
b3:
  Store %0, 1:Bool, aliasing=0, offset=17
  Jump block=b5
b4:
  Store %0, 0:Bool, aliasing=0, offset=18
  Jump block=b5
b5:
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
  Jump block=b3
b2:
  Jump block=b3
b3:
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

  return suite.finish();
}
