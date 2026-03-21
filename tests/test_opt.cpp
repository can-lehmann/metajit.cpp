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
  return 0;
}
