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

int main() {
  LLVMCodeGen::initilize_llvm_jit();

  GenExtTestSuite suite("tests/output");

  suite.gen_ext_test("add_promoted").run([](Builder& builder, TraceTestData& data) {
    Value* x = data.static_input(RandomRange(Type::Int32));  // promoted/frozen
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

  return suite.finish();
}
