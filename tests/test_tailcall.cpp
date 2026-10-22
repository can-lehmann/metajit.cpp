#include "../x86gen.hpp"
#include "../llvmgen.hpp"
#include "llvm/IR/Verifier.h"
#include "../../unittest.cpp/unittest.hpp"
#include <sstream>

using namespace metajit;
using namespace unittest;

struct TailState {
  uint64_t remaining;
  uint64_t minimum = UINT64_MAX;
  uint64_t maximum = 0;
  uint64_t sum = 0;
  uint64_t values[24];
};

int main(int argc, char** argv) {
  Suite suite(argc, argv);
  suite.test("tail transfer preserves spilled values and bounds stack growth").run([]() {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* entry = builder.build_block({Type::Ptr, Type::Ptr});
    builder.move_to_end(entry);
    Value* state = entry->arg(0);
    std::vector<Value*> values;
    for (size_t i = 0; i < 24; i++) {
      values.push_back(builder.build_load(state, Type::Int64, LoadFlags::None, 0, offsetof(TailState, values) + i * 8));
    }
    Value* local = builder.build_alloca(builder.build_const(Type::Int64, 256), 16);
    Value* address = builder.build_ptr_to_int(local, Type::Int64);
    Value* minimum = builder.build_load(state, Type::Int64, LoadFlags::None, 0, offsetof(TailState, minimum));
    Value* maximum = builder.build_load(state, Type::Int64, LoadFlags::None, 0, offsetof(TailState, maximum));
    builder.build_store(state, builder.build_select(builder.build_lt_u(address, minimum), address, minimum), 0, offsetof(TailState, minimum));
    builder.build_store(state, builder.build_select(builder.build_lt_u(maximum, address), address, maximum), 0, offsetof(TailState, maximum));
    Value* sum = builder.build_load(state, Type::Int64, LoadFlags::None, 0, offsetof(TailState, sum));
    for (Value* value : values) {
      sum = builder.build_add(sum, value);
    }
    builder.build_store(state, sum, 0, offsetof(TailState, sum));
    Value* remaining = builder.build_load(state, Type::Int64, LoadFlags::None, 0, offsetof(TailState, remaining));
    remaining = builder.build_sub(remaining, builder.build_const(Type::Int64, 1));
    builder.build_store(state, remaining, 0, offsetof(TailState, remaining));
    Block* again = builder.build_block();
    Block* done = builder.build_block();
    builder.build_branch(builder.build_eq(remaining, builder.build_const(Type::Int64, 0)), done, again);
    builder.move_to_end(again);
    Value* target = builder.build_load(entry->arg(1), Type::Ptr, LoadFlags::None, 0, 0);
    builder.build_call(target, Type::Void, {state, entry->arg(1)}, CallConv::PreserveNone, CallFlags::Tail);
    builder.build_exit();
    builder.move_to_end(done);
    builder.build_exit();
    OrderBlocks::run(&section, BlockOrdering::Natural);
    std::ostringstream errors;
    unittest_assert(!section.verify(errors));
    llvm::LLVMContext llvm_context;
    llvm::Module module("tailcall", llvm_context);
    LLVMCodeGen llvm_codegen(&section, &module, "tailcall");
    unittest_assert(!llvm::verifyModule(module, &llvm::errs()));
    unittest_assert(module.getFunction("tailcall")->getCallingConv() == llvm::CallingConv::PreserveNone);
    X86CodeGen codegen(&section, {Reg::X86_R12(), Reg::X86_R13()});
    using Function = void(* [[clang::preserve_none]])(TailState*, void**);
    void* target_code = codegen.deploy();
    TailState data{1000};
    for (size_t i = 0; i < 24; i++) {
      data.values[i] = i + 1;
    }
    reinterpret_cast<Function>(target_code)(&data, &target_code);
    unittest_assert(data.remaining == 0);
    unittest_assert(data.sum == 300000);
    unittest_assert(data.maximum == data.minimum);
  });
  suite.test("tail transfer between sections permutes arguments").run([]() {
    Context context;
    Allocator allocator;
    Section destination(context, allocator);
    Builder destination_builder(&destination);
    Block* destination_entry = destination_builder.build_block({Type::Ptr, Type::Ptr});
    destination_builder.move_to_end(destination_entry);
    Value* input = destination_builder.build_load(destination_entry->arg(1), Type::Int64, LoadFlags::None, 0, 0);
    destination_builder.build_store(destination_entry->arg(0), destination_builder.build_add(input, destination_builder.build_const(Type::Int64, 7)), 0, 0);
    destination_builder.build_exit();
    OrderBlocks::run(&destination, BlockOrdering::Natural);
    X86CodeGen destination_codegen(&destination, {Reg::X86_R12(), Reg::X86_R13()});
    void* target = destination_codegen.deploy();
    Section source(context, allocator);
    Builder builder(&source);
    Block* entry = builder.build_block({Type::Ptr, Type::Ptr});
    builder.move_to_end(entry);
    builder.build_call(builder.build_const(Type::Ptr, reinterpret_cast<uint64_t>(target)), Type::Void,
      {entry->arg(1), entry->arg(0)}, CallConv::PreserveNone, CallFlags::Tail);
    builder.build_exit();
    OrderBlocks::run(&source, BlockOrdering::Natural);
    std::ostringstream errors;
    unittest_assert(!source.verify(errors));
    X86CodeGen codegen(&source, {Reg::X86_R12(), Reg::X86_R13()});
    using Function = void(* [[clang::preserve_none]])(uint64_t*, uint64_t*);
    uint64_t value = 35;
    uint64_t result = 0;
    reinterpret_cast<Function>(codegen.deploy())(&value, &result);
    unittest_assert(value == 35);
    unittest_assert(result == 42);
  });
  suite.test("tail call verifier rejects invalid call shapes").run([]() {
    for (int invalid = 0; invalid < 5; invalid++) {
      Context context;
      Allocator allocator;
      Section section(context, allocator);
      Builder builder(&section);
      Block* entry = builder.build_block({Type::Ptr});
      builder.move_to_end(entry);
      CallFlags flags = CallFlags::Tail;
      if (invalid == 1) {
        flags = flags | CallFlags::Pure;
      }
      CallConv convention = invalid == 2 ? CallConv::Default : CallConv::PreserveNone;
      std::vector<Value*> arguments = {entry->arg(0)};
      if (invalid == 3) {
        arguments.clear();
      } else if (invalid == 4) {
        arguments[0] = builder.build_const(Type::Int64, 0);
      }
      builder.build_call(entry->arg(0), Type::Void, arguments, convention, flags);
      if (invalid == 0) {
        builder.build_store(entry->arg(0), builder.build_const(Type::Int64, 1), 0, 0);
      }
      builder.build_exit();
      std::ostringstream errors;
      unittest_assert(section.verify(errors));
    }
  });
  return suite.finish();
}
