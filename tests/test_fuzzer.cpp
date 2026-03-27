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

  DiffTestSuite suite("tests/output/test_fuzzer");

  // These are all the test cases that were found by the fuzzer

  suite.diff_test("select_large_int").run([](Builder& builder, TestData& data) {
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
  suite.diff_test("mov64_imm64_rexw").run([](Builder& builder, TestData& data) {
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

  suite.diff_test("shr_multiple_blocks").run([](Builder& builder, TestData& data) {
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

  suite.diff_test("resize_u_shl").run([](Builder& builder, TestData& data) {
    Value* a = data.input(Type::Int32);
    Value* b = data.input(RandomRange(Type::Int32, 0, 31));

    data.output(
      builder.build_resize_u(
        builder.build_shl(a, b),
        Type::Int64
      )
    );
  });

  suite.diff_test("bug_fold_lt_u").run([](Builder& builder, TestData& data) {
    Value* lt = builder.fold_lt_u(
      builder.build_const(Type::Int64, 1271752347623423UL),
      builder.build_const(Type::Int64, 2347782347823478UL));
    Block* true_block = builder.build_block();
    Block* false_block = builder.build_block();
    builder.fold_branch(lt, true_block, false_block);

    builder.move_to_end(true_block);
    data.output(lt);
    builder.build_exit();

    builder.move_to_end(false_block);
    data.output(lt);
    builder.build_exit();
  });

  suite.diff_test("bug_used_bits_transfer_shr_s").run([](Builder& builder, TestData& data) {
    Value* input8 = data.input(Type::Int8);
    Value* input64 = builder.build_resize_s(input8, Type::Int64);
    Value* shr = builder.build_shr_s(input64, builder.build_const(Type::Int64, 53));
    Value* shr2 = builder.build_shr_u(shr, builder.build_const(Type::Int64, 49));
    data.output(shr2);
    builder.build_exit();
    check_codegen_differential("", builder.section(), data, 1024, true);
  });


  return suite.finish();
}
