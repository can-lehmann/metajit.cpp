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

#include "../tv.hpp"

#include "../../unittest.cpp/unittest.hpp"

using namespace metajit;

int main() {
  unittest::Suite suite;

  suite.test("add").run([]() {
    Context context;
    Allocator allocator;
    Section* section = new Section(context, allocator);

    Value* add = nullptr;

    {
      Builder builder(section);
      builder.move_to_end(builder.build_block({
        Type::Int32, // a
        Type::Int32  // b
      }));

      Arg* a = builder.entry_arg(0);
      Arg* b = builder.entry_arg(1);

      add = builder.build_add(a, b);
      builder.build_exit();
    }
    
    {
      z3::context z3_context;
      z3::expr a = z3_context.bv_const("a", 32);
      z3::expr b = z3_context.bv_const("b", 32);
      tv::Z3CodeGen codegen(section, z3_context, {a, b});

      z3::solver solver(z3_context);
      solver.add(codegen.emit(add) != a + b);

      std::cout << solver << std::endl;

      assert(solver.check() == z3::unsat);
    }
  });

  return 0;
}