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

  return 0;
}