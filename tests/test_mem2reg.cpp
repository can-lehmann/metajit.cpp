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

using namespace metajit;
using namespace metajit::test;

int main() {
  OptTestSuite suite("tests/output/test_mem2reg");

  suite.opt_test("linear_store_load").run([](Builder& builder, TestData& data) {
    Value* alloca = builder.build_alloca(Type::Int64);
    builder.build_store(alloca, data.input(Type::Int64), AliasingGroup(0), 0);
    data.output(builder.build_load(alloca, Type::Int64, LoadFlags::None, AliasingGroup(0), 0));
  }, [&](Section* section) {
    Mem2Reg::run(section);
  });

  suite.opt_test("linear_store_load2").run([](Builder& builder, TestData& data) {
    Value* alloca_1 = builder.build_alloca(Type::Int64);
    Value* alloca_2 = builder.build_alloca(Type::Int64);
    builder.build_store(alloca_1, data.input(Type::Int64), AliasingGroup(0), 0);
    builder.build_store(alloca_2, builder.build_load(alloca_1, Type::Int64, LoadFlags::None, AliasingGroup(0), 0), AliasingGroup(0), 0);
    data.output(builder.build_load(alloca_2, Type::Int64, LoadFlags::None, AliasingGroup(0), 0));
  }, [&](Section* section) {
    Mem2Reg::run(section);
  });

  suite.opt_test("linear_add").run([](Builder& builder, TestData& data) {
    Value* a = data.input(Type::Int64);
    Value* b = data.input(Type::Int64);

    Value* a_alloca = builder.build_alloca(Type::Int64);
    Value* b_alloca = builder.build_alloca(Type::Int64);
    Value* c_alloca = builder.build_alloca(Type::Int64);

    builder.build_store(a_alloca, a, AliasingGroup(0), 0);
    builder.build_store(b_alloca, b, AliasingGroup(0), 0);

    builder.build_store(c_alloca, builder.build_add(
      builder.build_load(a_alloca, Type::Int64, LoadFlags::None, AliasingGroup(0), 0),
      builder.build_load(b_alloca, Type::Int64, LoadFlags::None, AliasingGroup(0), 0)
    ), AliasingGroup(0), 0);

    data.output(builder.build_load(c_alloca, Type::Int64, LoadFlags::None, AliasingGroup(0), 0));
  }, [&](Section* section){
    Mem2Reg::run(section);
  });

  return suite.finish();
}