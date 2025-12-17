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

const std::string output_path = "tests/output/test_fuzzer";

int main() {
  metajit::LLVMCodeGen::initilize_llvm_jit();

  // These are all the test cases that were found by the fuzzer

  DiffTest("select_large_int", output_path).run([](Builder& builder, TestData& data) {
    data.output(
      builder.build_select(
        data.input(Type::Bool),
        builder.build_const(Type::Int64, 1271752347623423UL),
        builder.build_const(Type::Int64, 2347782347823478UL)
      )
    );
  });

  
  // TODO: This test depends on the behavior of the register allocator
  // it would be nice to test the instruction encoding of
  //   mov64_imm64 r8, 14624083866164270481
  // directly. Not a priority right now though.
  DiffTest("mov64_imm64_rexw", output_path).run([](Builder& builder, TestData& data) {
    Value* a = data.input(Type::Int32);
    Value* b = data.input(Type::Int32);
    Value* c = data.input(Type::Int32);
    Value* d = data.input(Type::Int32);
    Value* f = data.input(Type::Int64);

    data.output(
      builder.build_select(
        builder.build_lt_u(a, b),
        f,
        builder.build_select(
          builder.build_lt_u(c, d),
          builder.build_const(Type::Int64, 14624083866164270481UL),
          builder.build_const(Type::Int64, 14624083866164270480UL)
        )
      )
    );
  });

  DiffTest("shr_multiple_blocks", output_path).run([](Builder& builder, TestData& data) {
    Value* a = data.input(Type::Bool);
    Value* b = data.input(Type::Int8);
    Value* c = data.input(Type::Int8);
    Value* d = data.input(Type::Int8);
    Value* e = data.input(Type::Int8);

    Value* select = builder.build_select(a, b, c);
    Value* xor_value = builder.build_xor(d, e);
    Value* cmp = builder.build_lt_u(builder.build_const(Type::Int8, 7), xor_value);

    Block* true_block = builder.build_block();
    Block* false_block = builder.build_block();
    builder.fold_branch(cmp, true_block, false_block);

    builder.move_to_end(true_block);
    builder.build_exit();

    builder.move_to_end(false_block);
    data.output(builder.build_shr_u(select, xor_value));
    builder.build_exit();
  });


  return 0;
}