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

  GenExtTestSuite suite("tests/output/test_genext");

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

  suite.gen_ext_test("bug_poison").samples(4, 16).run([](Builder& builder, TraceTestData& data) {
    Value* cond = data.static_input(RandomRange(Type::Bool));
    Value* cond_resized = builder.build_resize_u(cond, Type::Int32);
    Value* shl1 = builder.build_shl(builder.build_const(Type::Int32, 0), cond_resized);
    Value* xor_val = builder.build_xor(shl1, builder.build_const(Type::Int32, 4294967295u));
    // shl2 is poison
    Value* shl2 = builder.build_shl(builder.build_const(Type::Int32, 0), xor_val);
    Value* final_select = builder.build_select(cond, builder.build_const(Type::Int32, 0), shl2);
    data.output(final_select);
  });

  suite.gen_ext_test("bug_promote_ptr").run([](Builder& builder, TraceTestData& data) {
    // This test should trigger the bug where Promote on a pointer type
    // generates invalid LLVM IR (zext ptr instead of ptrtoint)
    Value* ptr = data.static_input(RandomRange(Type::Ptr));  // frozen pointer
    Value* offset = data.input(RandomRange(Type::Int64));    // dynamic offset
    Value* result = builder.build_add_ptr(ptr, offset);
    data.output(result);
  });

  return suite.finish();
}
