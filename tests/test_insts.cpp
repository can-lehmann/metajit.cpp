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

int main() {
  using namespace metajit;
  using namespace metajit::test;

  LLVMCodeGen::initilize_llvm_jit();

  const std::string output_path = "tests/output/test_insts";

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

  binop(add, false)
  binop(sub, false)
  binop(mul, false)
  //div_mod(div_u)
  //div_mod(div_s)
  div_mod(mod_u)
  //div_mod(mod_s)
  binop(and, false)
  binop(or, false)
  binop(xor, false)
  shift(shr_u)
  shift(shr_s)
  shift(shl)
  binop(eq, false)
  binop(lt_u, false)
  binop(lt_s, false)
}