#include "../x86gen.hpp"
#include "../llvmgen.hpp"
#include "llvm/IR/Verifier.h"
#include "../../unittest.cpp/unittest.hpp"
#include <sstream>

using namespace metajit;
using namespace unittest;

__attribute__((noinline)) uint64_t stack_address() {
  return (uint64_t) __builtin_frame_address(0);
}

struct TailState {
  uint64_t remaining;
  uint64_t minimum = UINT64_MAX;
  uint64_t maximum = 0;
  uint64_t sum = 0;
  uint64_t values[24];
};

__attribute__((preserve_none)) void replacement_tail(uint64_t* result, uint64_t* value) {
  *result = *value + 70;
}

void tail_exit_with_values_live_in_other_branch() {
  Context context;
  Allocator allocator;
  Section section(context, allocator);
  Builder builder(&section);
  Block* entry = builder.build_block({Type::Ptr, Type::Ptr});
  builder.move_to_end(entry);
  std::vector<Value*> values;
  for (size_t index = 0; index < 32; index++) {
    values.push_back(builder.build_load(entry->arg(0), Type::Int64, LoadFlags::None, 0, index * 8));
  }
  Block* transfer = builder.build_block();
  Block* finish = builder.build_block();
  builder.build_branch(builder.build_eq(values[0], builder.build_const(Type::Int64, 0)), transfer, finish);
  builder.move_to_end(transfer);
  builder.build_call(builder.build_const(Type::Ptr, (uint64_t) replacement_tail), Type::Void,
    {entry->arg(0), entry->arg(1)}, CallConv::PreserveNone, CallFlags::Tail);
  builder.build_exit();
  builder.move_to_end(finish);
  Value* sum = values[0];
  for (size_t index = 1; index < values.size(); index++) {
    sum = builder.build_add(sum, values[index]);
  }
  builder.build_store(entry->arg(1), sum, 0, 0);
  builder.build_exit();
  OrderBlocks::run(&section, BlockOrdering::Natural);
  X86CodeGen codegen(&section, {Reg::X86_R12(), Reg::X86_R13()});
  using Function = void(* [[clang::preserve_none]])(uint64_t*, uint64_t*);
  Function function = (Function) codegen.deploy();
  uint64_t data[32] = {};
  uint64_t result = 5;
  function(data, &result);
  unittest_assert(data[0] == 75);
  function(data, &result);
  unittest_assert(result == 75);
}

void tail_stack_growth() {
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
  Value* address = builder.build_call(builder.build_const(Type::Ptr, (uint64_t) stack_address), Type::Int64, std::vector<Value*>{});
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
}
void tail_argument_permutation() {
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
}
void tail_invalid_shapes() {
  for (int invalid = 0; invalid < 7; invalid++) {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* entry = builder.build_block({Type::Ptr});
    builder.move_to_end(entry);
    CallFlags flags = CallFlags::Tail;
    if (invalid == 1) {
      flags = flags | CallFlags::Pure;
    } else if (invalid == 6) {
      flags = flags | CallFlags::Idempotent;
    }
    CallConv convention = invalid == 2 ? CallConv::Default : CallConv::PreserveNone;
    std::vector<Value*> arguments = {entry->arg(0)};
    if (invalid == 3) {
      arguments.clear();
    } else if (invalid == 4) {
      arguments[0] = builder.build_const(Type::Int64, 0);
    }
    builder.build_call(entry->arg(0), invalid == 5 ? Type::Int64 : Type::Void, arguments, convention, flags);
    if (invalid == 0) {
      builder.build_store(entry->arg(0), builder.build_const(Type::Int64, 1), 0, 0);
    }
    builder.build_exit();
    std::ostringstream errors;
    unittest_assert(section.verify(errors));
  }
}

void tail_alloca_rejected() {
  for (bool allocate_first : {false, true}) {
    for (bool tail : {false, true}) {
      Context context;
      Allocator allocator;
      Section section(context, allocator);
      Builder builder(&section);
      Block* entry = builder.build_block({Type::Ptr});
      Block* allocation = allocate_first ? entry : builder.build_block();
      Block* transfer = allocate_first ? builder.build_block() : entry;
      builder.move_to_end(allocation);
      builder.build_alloca(builder.build_const(Type::Int64, 16), 8);
      if (allocate_first) {
        builder.build_jump(transfer);
      } else {
        builder.build_exit();
      }
      builder.move_to_end(transfer);
      builder.build_call(entry->arg(0), Type::Void, {entry->arg(0)}, CallConv::PreserveNone,
        tail ? CallFlags::Tail : CallFlags::None);
      builder.build_exit();
      std::ostringstream errors;
      unittest_assert(section.verify(errors) == tail);
      if (tail) {
        unittest_assert(errors.str().find("Alloca") != std::string::npos);
      }
    }
  }
}

int main(int argc, char** argv) {
  Suite suite(argc, argv);
  suite.test("tail exit with values live in another branch").run(tail_exit_with_values_live_in_other_branch);
  suite.test("tail calls reject alloca anywhere in the section").run(tail_alloca_rejected);
  suite.test("tail transfer preserves spilled values and bounds stack growth").run(tail_stack_growth);
  suite.test("tail transfer between sections permutes arguments").run(tail_argument_permutation);
  suite.test("tail call verifier rejects invalid call shapes").run(tail_invalid_shapes);
  return suite.finish();
}
