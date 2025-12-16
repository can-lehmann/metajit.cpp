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

  #define binop(name, supports_bool) \
    DiffTest(#name).run([](Builder& builder, TestData& data) { \
      if (supports_bool) { \
        data.output(builder.build_##name(data.input(Type::Bool), data.input(Type::Bool))); \
      } \
      data.output(builder.build_##name(data.input(Type::Int8), data.input(Type::Int8))); \
      data.output(builder.build_##name(data.input(Type::Int16), data.input(Type::Int16))); \
      data.output(builder.build_##name(data.input(Type::Int32), data.input(Type::Int32))); \
      data.output(builder.build_##name(data.input(Type::Int64), data.input(Type::Int64))); \
    });
  
  binop(add, false)
  binop(sub, false)
  binop(mul, false)
  //binop(div_u, false)
  //binop(div_s, false)
  binop(mod_u, false)
  //binop(mod_s, false)
  binop(and, false)
  binop(or, false)
  binop(xor, false)
  binop(shr_u, false)
  binop(shr_s, false)
  binop(shl, false)
  binop(eq, false)
  binop(lt_u, false)
  binop(lt_s, false)
}