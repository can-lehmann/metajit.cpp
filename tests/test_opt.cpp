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


const std::string output_path = "tests/output/test_opt";

void check_simplify(const std::string& expected, Section* section) {
  metajit::Simplify::run(section, 1);
  std::stringstream ss;
  section->write(ss);
  if (ss.str() != expected) {
    std::cerr << "Expected:\n" << expected << "\n\nGot:\n" << ss.str() << std::endl;
  }
  unittest_assert(ss.str() == expected);
}

int main() {
  metajit::LLVMCodeGen::initilize_llvm_jit();
  DiffTest("resize_resize_to_mask", output_path).run([](Builder& builder, TestData& data) {

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
  %3 = And %1, 255
  Store %0, %3, aliasing=0, offset=8
}
)", builder.section());
  });

  DiffTest("shru_and_shl_to_and", output_path).run([](Builder& builder, TestData& data) {

    Value* input = data.input(Type::Int64);
    Value* shifted = builder.fold_shr_u(input, builder.build_const(Type::Int64, 1));
    Value* anded = builder.fold_and(shifted, builder.build_const(Type::Int64, 3));
    Value* back = builder.fold_shl(anded, builder.build_const(Type::Int64, 1));
    data.output(back);

    // it's really the smart constructors that do this.
    check_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int64, flags={}, aliasing=0, offset=0
  %2 = ShrU %1, 1
  %3 = And %1, 6
  Store %0, %3, aliasing=0, offset=8
}
)", builder.section());
  });

  DiffTest("shru_and_shl_to_shr", output_path).run([](Builder& builder, TestData& data) {

    Value* input = data.input(Type::Int64);
    Value* shifted = builder.fold_shr_u(input, builder.build_const(Type::Int64, 10));
    Value* anded = builder.fold_and(shifted, builder.build_const(Type::Int64, 1));
    Value* back = builder.fold_shl(anded, builder.build_const(Type::Int64, 1));
    data.output(back);

    // it's really the smart constructors that do this.
    check_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int64, flags={}, aliasing=0, offset=0
  %2 = ShrU %1, 10
  %3 = ShrU %1, 9
  %4 = And %3, 2
  Store %0, %4, aliasing=0, offset=8
}
)", builder.section());
  });

  DiffTest("shru_and_shl_to_shl", output_path).run([](Builder& builder, TestData& data) {

    Value* input = data.input(Type::Int64);
    Value* shifted = builder.fold_shr_u(input, builder.build_const(Type::Int64, 1));
    Value* anded = builder.fold_and(shifted, builder.build_const(Type::Int64, 1));
    Value* back = builder.fold_shl(anded, builder.build_const(Type::Int64, 3));
    data.output(back);

    check_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int64, flags={}, aliasing=0, offset=0
  %2 = ShrU %1, 1
  %3 = Shl %1, 2
  %4 = And %3, 8
  Store %0, %4, aliasing=0, offset=8
}
)", builder.section());
  });


  DiffTest("shl_and_shl_to_shl", output_path).run([](Builder& builder, TestData& data) {

    Value* input = data.input(Type::Int64);
    Value* shifted = builder.fold_shl(input, builder.build_const(Type::Int64, 1));
    Value* anded = builder.fold_and(shifted, builder.build_const(Type::Int64, 0xff));
    Value* back = builder.fold_shl(anded, builder.build_const(Type::Int64, 3));
    data.output(back);

    check_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int64, flags={}, aliasing=0, offset=0
  %2 = Shl %1, 1
  %3 = Shl %1, 4
  %4 = And %3, 2040
  Store %0, %4, aliasing=0, offset=8
}
)", builder.section());
  });

  DiffTest("or_and_and_to_or", output_path).run([](Builder& builder, TestData& data) {
    Value* input = data.input(Type::Int64);
    Value* part1 = builder.fold_and(input, builder.build_const(Type::Int64, 3));
    Value* part2 = builder.fold_and(input, builder.build_const(Type::Int64, 12));
    Value* result = builder.fold_or(part1, part2);
    data.output(result);

    check_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int64, flags={}, aliasing=0, offset=0
  %2 = And %1, 15
  Store %0, %2, aliasing=0, offset=8
}
)", builder.section());
  });

  DiffTest("and_or_and_shortcut", output_path).run([](Builder& builder, TestData& data) {
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
  %4 = And %2, 4294902015
  Store %0, %4, aliasing=0, offset=16
}
)", builder.section());
  });

  DiffTest("select_and_knownbits", output_path).run([](Builder& builder, TestData& data) {

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

  DiffTest("and_idempotent_not_constant", output_path).run([](Builder& builder, TestData& data) {
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
  %3 = And %1, 240
  Store %0, %3, aliasing=0, offset=2
}
)", builder.section());
  });

  DiffTest("or_idempotent", output_path).run([](Builder& builder, TestData& data) {
    Value* value = data.input(Type::Int16);
    Value* value2 = builder.build_or(value, builder.build_const(Type::Int16, 0b11));
    // the second or is unnecessary
    Value* value3 = builder.build_or(value2, builder.build_const(Type::Int16, 0b11));
    data.output(value3);
    check_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int16, flags={}, aliasing=0, offset=0
  %2 = Or %1, 3
  Store %0, %2, aliasing=0, offset=2
}
)", builder.section());
  });

  DiffTest("or_and_one_component", output_path).run([](Builder& builder, TestData& data) {
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
  %4 = Shl %2, 14
  %5 = Or %3, %1
  %6 = Or %5, %4
  %7 = And %1, 4095
  Store %0, %7, aliasing=0, offset=6
}
)", builder.section());
  });

  return 0;
}
