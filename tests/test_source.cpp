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

int main(int argc, char** argv) {
  LLVMCodeGen::initilize_llvm_jit();

  SourceTestSuite suite("tests/output/test_source", argc, argv);

  suite.test("negative_gep_index").run([]() {
    llvm::LLVMContext llvm_context;
    llvm::SMDiagnostic error;
    auto module = llvm::parseIR(llvm::MemoryBufferRef(R"(
      define void @run(ptr %p, i32 %i, ptr %out) {
        %q = getelementptr i8, ptr %p, i32 %i
        store ptr %q, ptr %out
        ret void
      }
    )", "negative_gep_index"), error, llvm_context);
    unittest_assert(module != nullptr);
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    LowerLLVM(module->getFunction("run"), &section);
    char buffer[2];
    void* result = nullptr;
    Interpreter interpreter(&section, {
      Interpreter::Bits::constant(buffer + 1),
      Interpreter::Bits::constant(Type::Int32, uint32_t(-1)),
      Interpreter::Bits::constant(&result)
    });
    unittest_assert(interpreter.run() == Interpreter::Event::Exit);
    unittest_assert(result == buffer);
  });

  suite.source_test("tests/source/fib.o0.ll")
    .inputs({ RandomRange(Type::Int32, 0, 40) })
    .outputs({ Type::Int32 })
    .run();

  suite.source_test("tests/source/fib.o1.ll")
    .inputs({ RandomRange(Type::Int32, 0, 40) })
    .outputs({ Type::Int32 })
    .run();

  for (const char* path : {"tests/source/bit_manipulation.o0.ll", "tests/source/bit_manipulation.o1.ll"}) {
    suite.source_test(path)
      .inputs({ RandomRange(Type::Int8), RandomRange(Type::Int16), RandomRange(Type::Int32), RandomRange(Type::Int64) })
      .outputs({ Type::Int8, Type::Int16, Type::Int32, Type::Int64, Type::Int8, Type::Int16, Type::Int32, Type::Int64 })
      .run();
  }

  return suite.finish();
}
