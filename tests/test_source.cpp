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

template <size_t Width>
void check_funnel_lowering() {
  llvm::LLVMContext llvm_context;
  llvm::Module module("funnel", llvm_context);
  module.setDataLayout("e-p:64:64");
  llvm::IRBuilder<> ir(llvm_context);
  llvm::IntegerType* type = ir.getIntNTy(Width);
  llvm::Function* function = llvm::Function::Create(
    llvm::FunctionType::get(ir.getVoidTy(), {ir.getPtrTy()}, false),
    llvm::Function::ExternalLinkage, "run", module);
  ir.SetInsertPoint(llvm::BasicBlock::Create(llvm_context, "entry", function));
  llvm::Value* data = function->getArg(0);
  llvm::Value* a = ir.CreateFreeze(ir.CreateLoad(type, data));
  llvm::Value* b = ir.CreateLoad(type, ir.CreateGEP(ir.getInt8Ty(), data, ir.getInt64(8)));
  llvm::Value* amount = ir.CreateLoad(type, ir.CreateGEP(ir.getInt8Ty(), data, ir.getInt64(16)));
  std::vector<uint64_t> amounts = {0, 1, Width - 1, Width, Width + 1, ~uint64_t(0)};
  size_t output = 0;
  for (llvm::Intrinsic::ID id : {llvm::Intrinsic::fshl, llvm::Intrinsic::fshr}) {
    for (size_t index = 0; index <= amounts.size(); index++) {
      llvm::Value* shift = index == 0 ? amount : ir.getIntN(Width, amounts[index - 1]);
      llvm::Value* result = ir.CreateIntrinsic(id, {type}, {a, b, shift});
      ir.CreateStore(result, ir.CreateGEP(ir.getInt8Ty(), data, ir.getInt64(24 + output++ * 8)));
    }
  }
  ir.CreateRetVoid();
  Context context;
  Allocator allocator;
  Section section(context, allocator);
  LowerLLVM(function, &section);
  OrderBlocks::run(&section, BlockOrdering::Natural);
  unittest_assert(!section.verify(std::cout));
  X86CodeGen codegen(&section, {Reg::phys(12)}, X86CodeGen::Mode::JIT);
  using Fn = void(* [[clang::preserve_none]])(uint8_t*);
  Fn fn = (Fn) codegen.deploy();
  uint64_t random = 0x123456789abcdef;
  uint64_t mask = type_mask(int_type_of_width(Width));
  for (size_t sample = 0; sample < 1024; sample++) {
    uint64_t values[24] = {};
    for (size_t index = 0; index < 3; index++) {
      random ^= random << 13;
      random ^= random >> 7;
      random ^= random << 17;
      values[index] = random;
    }
    uint64_t x = values[0] & mask;
    uint64_t y = values[1] & mask;
    uint64_t shift = values[2];
    fn((uint8_t*) values);
    output = 0;
    for (bool right : {false, true}) {
      for (size_t index = 0; index <= amounts.size(); index++) {
        size_t count = (index == 0 ? shift : amounts[index - 1]) % Width;
        uint64_t expected = right ? y : x;
        if (count) {
          expected = right ? (x << (Width - count)) | (y >> count) :
                             (x << count) | (y >> (Width - count));
        }
        unittest_assert((values[3 + output++] & mask) == (expected & mask));
      }
    }
  }
}

int main(int argc, char** argv) {
  LLVMCodeGen::initilize_llvm_jit();

  SourceTestSuite suite("tests/output/test_source", argc, argv);

  suite.test("funnel_Int8").run(check_funnel_lowering<8>);
  suite.test("funnel_Int16").run(check_funnel_lowering<16>);
  suite.test("funnel_Int32").run(check_funnel_lowering<32>);
  suite.test("funnel_Int64").run(check_funnel_lowering<64>);

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
