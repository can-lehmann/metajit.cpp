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

const std::string output_path = "tests/output/test_insts";

void test_binop() {
  #define binop_type(name, type) \
    DiffTest(#name "_" #type, output_path).run([](Builder& builder, TestData& data) { \
      data.output(builder.build_##name(data.input(Type::type), data.input(Type::type))); \
    }); \
    DiffTest(#name "_" #type "_imm", output_path).run([](Builder& builder, TestData& data) { \
      data.output(builder.build_##name(data.input(Type::type), RandomRange(Type::type).gen_const(builder))); \
    });

  #define binop(name, supports_bool) \
    if (supports_bool) { binop_type(name, Bool); } \
    binop_type(name, Int8); \
    binop_type(name, Int16); \
    binop_type(name, Int32); \
    binop_type(name, Int64);
  
  binop(add, false)
  binop(sub, false)
  binop(mul, false)

  binop(and, false)
  binop(or, false)
  binop(xor, false)

  binop(eq, false)
  binop(lt_u, false)
  binop(lt_s, false)
}

void test_shift() {
  #define shift_type(name, type) \
    DiffTest(#name "_" #type, output_path).run([](Builder& builder, TestData& data) { \
      Value* by = data.input(RandomRange(Type::type, 0, type_size(Type::type) * 8 - 1)); \
      data.output(builder.build_##name(data.input(Type::type), by)); \
    }); \
    DiffTest(#name "_" #type "_imm", output_path).run([](Builder& builder, TestData& data) { \
      Value* by = RandomRange(Type::type, 0, type_size(Type::type) * 8 - 1).gen_const(builder); \
      data.output(builder.build_##name(data.input(Type::type), by)); \
    });

  #define shift(name) \
    shift_type(name, Int8); \
    shift_type(name, Int16); \
    shift_type(name, Int32); \
    shift_type(name, Int64);

  shift(shr_u)
  shift(shr_s)
  shift(shl)
}

void test_div_mod() {
  #define div_mod_type(name, type) \
    DiffTest(#name "_" #type, output_path).run([](Builder& builder, TestData& data) { \
      Value* divisor = data.input(RandomRange(Type::type, 1, type_mask(Type::type))); \
      data.output(builder.build_##name(data.input(Type::type), divisor)); \
    });

  #define div_mod(name) \
    div_mod_type(name, Int8); \
    div_mod_type(name, Int16); \
    div_mod_type(name, Int32); \
    div_mod_type(name, Int64);

  //div_mod(div_u)
  //div_mod(div_s)
  div_mod(mod_u)
  //div_mod(mod_s)
}

void test_select() {
  #define select_type(type) \
    DiffTest("select_" #type, output_path).run([](Builder& builder, TestData& data) { \
      data.output(builder.build_select( \
        data.input(Type::Bool), \
        data.input(Type::type), \
        data.input(Type::type) \
      )); \
    });
  
  select_type(Bool)
  select_type(Int8)
  select_type(Int16)
  select_type(Int32)
  select_type(Int64)
}

void test_resize() {
  #define resize_type(name, from_type, to_type) \
    DiffTest(#name "_" #from_type "_to_" #to_type, output_path).run([](Builder& builder, TestData& data) { \
      data.output(builder.build_##name(data.input(Type::from_type), Type::to_type)); \
    });
  
  #define resize(name) \
    resize_type(name, Bool, Int8) \
    resize_type(name, Bool, Int16) \
    resize_type(name, Bool, Int32) \
    resize_type(name, Bool, Int64) \
    \
    resize_type(name, Int8, Bool) \
    resize_type(name, Int8, Int16) \
    resize_type(name, Int8, Int32) \
    resize_type(name, Int8, Int64) \
    \
    resize_type(name, Int16, Bool) \
    resize_type(name, Int16, Int8) \
    resize_type(name, Int16, Int32) \
    resize_type(name, Int16, Int64) \
    \
    resize_type(name, Int32, Bool) \
    resize_type(name, Int32, Int8) \
    resize_type(name, Int32, Int16) \
    resize_type(name, Int32, Int64) \
    \
    resize_type(name, Int64, Bool) \
    resize_type(name, Int64, Int8) \
    resize_type(name, Int64, Int16) \
    resize_type(name, Int64, Int32)
  
  resize(resize_u)
  resize(resize_s)
}

int main() {
  LLVMCodeGen::initilize_llvm_jit();

  test_binop();
  test_shift();
  test_div_mod();
  test_select();
  test_resize();

  return 0;
}