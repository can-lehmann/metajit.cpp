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

void test_binop(DiffTestSuite& suite) {
  #define binop_type(name, type) \
    suite.diff_test(#name "_" #type).run([](Builder& builder, TestData& data) { \
      data.output(builder.build_##name(data.input(Type::type), data.input(Type::type))); \
    }); \
    suite.diff_test(#name "_" #type "_imm").run([](Builder& builder, TestData& data) { \
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

void test_shift(DiffTestSuite& suite) {
  #define shift_type(name, type) \
    suite.diff_test(#name "_" #type).run([](Builder& builder, TestData& data) { \
      Value* by = data.input(RandomRange(Type::type, 0, type_size(Type::type) * 8 - 1)); \
      data.output(builder.build_##name(data.input(Type::type), by)); \
    }); \
    suite.diff_test(#name "_" #type "_imm").run([](Builder& builder, TestData& data) { \
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

void test_div_mod(DiffTestSuite& suite) {
  #define div_mod_type(name, type) \
    suite.diff_test(#name "_" #type).run([](Builder& builder, TestData& data) { \
      Value* divisor = data.input(RandomRange(Type::type, 1, type_mask(Type::type))); \
      data.output(builder.build_##name(data.input(Type::type), divisor)); \
    });

  #define div_mod(name) \
    div_mod_type(name, Int8); \
    div_mod_type(name, Int16); \
    div_mod_type(name, Int32); \
    div_mod_type(name, Int64);

  div_mod(div_u)
  div_mod(div_s)
  div_mod(mod_u)
  div_mod(mod_s)
}

void test_select(DiffTestSuite& suite) {
  #define select_type(type) \
    suite.diff_test("select_" #type).run([](Builder& builder, TestData& data) { \
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

void test_resize(DiffTestSuite& suite) {
  #define resize_type(name, from_type, to_type) \
    suite.diff_test(#name "_" #from_type "_to_" #to_type).run([](Builder& builder, TestData& data) { \
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

static uint32_t test_call_default_void_slot = 0;
static uint64_t test_call_ptr_anchor = 0;

extern "C" __attribute__((preserve_none, noinline))
uint64_t test_call_preserve_none_target(uint64_t a, uint64_t b, uint64_t c) {
  return (a + b) ^ c;
}

extern "C" __attribute__((preserve_none, noinline))
uint64_t test_call_preserve_none_target_0() {
  return 0x9e3779b97f4a7c15ULL;
}

extern "C" __attribute__((preserve_none, noinline))
uint64_t test_call_preserve_none_target_1(uint64_t a) {
  return (a * 7) ^ 0x1234;
}

extern "C" __attribute__((preserve_none, noinline))
uint64_t test_call_preserve_none_target_4(uint64_t a,
                                          uint64_t b,
                                          uint64_t c,
                                          uint64_t d) {
  return (a + b) - (c ^ d);
}

extern "C" __attribute__((noinline))
uint64_t test_call_default_target(uint64_t a, uint64_t b, uint64_t c) {
  return (a - b) + (c * 5);
}

extern "C" __attribute__((noinline))
uint64_t test_call_default_target_0() {
  return 0x123456789abcdef0ULL;
}

extern "C" __attribute__((noinline))
uint64_t test_call_default_target_1(uint64_t a) {
  return (a ^ 0x55aa55aa55aa55aaULL) + 17;
}

extern "C" __attribute__((noinline))
uint64_t test_call_default_target_2(uint64_t a, uint64_t b) {
  return (a + (b << 1)) ^ 0x1020304050607080ULL;
}

extern "C" __attribute__((noinline))
uint64_t test_call_default_target_4(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
  return (a ^ b) + (c ^ d);
}

extern "C" __attribute__((noinline))
uint64_t test_call_default_mixed_types(bool b, uint8_t i8, uint16_t i16, uint32_t i32, void* ptr) {
  uint64_t bv = b ? 1 : 0;
  uint64_t pv = (uint64_t)(uintptr_t) ptr;
  return bv + (uint64_t) i8 + (uint64_t) i16 + (uint64_t) i32 + (pv & 0xff);
}

extern "C" __attribute__((noinline))
void* test_call_default_ptr_id(void* ptr) {
  return ptr;
}

extern "C" __attribute__((noinline))
void test_call_default_void_store(uint32_t* out, uint32_t a, uint32_t b) {
  *out = a + b + 1;
}

void test_call(DiffTestSuite& suite) {
  suite.diff_test("call_preserve_none").interpreter(false).run([](Builder& builder, TestData& data) {
    Value* a = data.input(Type::Int64);
    Value* b = data.input(Type::Int64);
    Value* c = data.input(Type::Int64);

    Value* callee = builder.build_const(Type::Ptr, (uint64_t)(void*) test_call_preserve_none_target);
    Value* result = builder.build_call(
      callee,
      Type::Int64,
      std::vector<Value*>({a, b, c}),
      CallConv::PreserveNone
    );

    data.output(result);
  });

  suite.diff_test("call_preserve_none_0").interpreter(false).run([](Builder& builder, TestData& data) {
    Value* callee = builder.build_const(Type::Ptr, (uint64_t)(void*) test_call_preserve_none_target_0);
    Value* result = builder.build_call(callee, Type::Int64, std::vector<Value*>(), CallConv::PreserveNone);
    data.output(result);
  });

  suite.diff_test("call_preserve_none_1_smoke").interpreter(false).run([](Builder& builder, TestData& data) {
    Value* a = data.input(Type::Int64);

    Value* callee = builder.build_const(Type::Ptr, (uint64_t)(void*) test_call_preserve_none_target_1);
    builder.build_call(
      callee,
      Type::Int64,
      std::vector<Value*>({a}),
      CallConv::PreserveNone
    );
  });

  suite.diff_test("call_preserve_none_4_smoke").interpreter(false).run([](Builder& builder, TestData& data) {
    Value* a = data.input(Type::Int64);
    Value* b = data.input(Type::Int64);
    Value* c = data.input(Type::Int64);
    Value* d = data.input(Type::Int64);

    Value* callee = builder.build_const(Type::Ptr, (uint64_t)(void*) test_call_preserve_none_target);
    builder.build_call(
      callee,
      Type::Int64,
      std::vector<Value*>({a, b, c, d}),
      CallConv::PreserveNone
    );
  });

  suite.diff_test("call_default").interpreter(false).run([](Builder& builder, TestData& data) {
    Value* a = data.input(Type::Int64);
    Value* b = data.input(Type::Int64);
    Value* c = data.input(Type::Int64);

    Value* callee = builder.build_const(Type::Ptr, (uint64_t)(void*) test_call_default_target);
    Value* result = builder.build_call(
      callee,
      Type::Int64,
      std::vector<Value*>({a, b, c}),
      CallConv::Default
    );

    data.output(result);
  });

  suite.diff_test("call_default_0").interpreter(false).run([](Builder& builder, TestData& data) {
    Value* callee = builder.build_const(Type::Ptr, (uint64_t)(void*) test_call_default_target_0);
    Value* result = builder.build_call(callee, Type::Int64, std::vector<Value*>(), CallConv::Default);
    data.output(result);
  });

  suite.diff_test("call_default_1").interpreter(false).run([](Builder& builder, TestData& data) {
    Value* a = data.input(Type::Int64);
    Value* callee = builder.build_const(Type::Ptr, (uint64_t)(void*) test_call_default_target_1);
    Value* result = builder.build_call(callee, Type::Int64, std::vector<Value*>({a}), CallConv::Default);
    data.output(result);
  });

  suite.diff_test("call_default_2").interpreter(false).run([](Builder& builder, TestData& data) {
    Value* a = data.input(Type::Int64);
    Value* b = data.input(Type::Int64);
    Value* callee = builder.build_const(Type::Ptr, (uint64_t)(void*) test_call_default_target_2);
    Value* result = builder.build_call(callee, Type::Int64, std::vector<Value*>({a, b}), CallConv::Default);
    data.output(result);
  });

  suite.diff_test("call_default_4").interpreter(false).run([](Builder& builder, TestData& data) {
    Value* a = data.input(Type::Int64);
    Value* b = data.input(Type::Int64);
    Value* c = data.input(Type::Int64);
    Value* d = data.input(Type::Int64);
    Value* callee = builder.build_const(Type::Ptr, (uint64_t)(void*) test_call_default_target_4);
    Value* result = builder.build_call(callee, Type::Int64, std::vector<Value*>({a, b, c, d}), CallConv::Default);
    data.output(result);
  });

  suite.diff_test("call_default_mixed_types").interpreter(false).run([](Builder& builder, TestData& data) {
    Value* b = data.input(Type::Bool);
    Value* i8 = data.input(Type::Int8);
    Value* i16 = data.input(Type::Int16);
    Value* i32 = data.input(Type::Int32);
    Value* ptr = builder.build_const(Type::Ptr, (uint64_t)(void*) &test_call_ptr_anchor);

    Value* callee = builder.build_const(Type::Ptr, (uint64_t)(void*) test_call_default_mixed_types);
    Value* result = builder.build_call(
      callee,
      Type::Int64,
      std::vector<Value*>({b, i8, i16, i32, ptr}),
      CallConv::Default
    );
    data.output(result);
  });

  suite.diff_test("call_default_ptr_ret").interpreter(false).run([](Builder& builder, TestData& data) {
    Value* ptr = builder.build_const(Type::Ptr, (uint64_t)(void*) &test_call_ptr_anchor);
    Value* callee = builder.build_const(Type::Ptr, (uint64_t)(void*) test_call_default_ptr_id);
    Value* result = builder.build_call(
      callee,
      Type::Ptr,
      std::vector<Value*>({ptr}),
      CallConv::Default
    );
    data.output(result);
  });

  suite.diff_test("call_default_void_ret").interpreter(false).run([](Builder& builder, TestData& data) {
    Value* out_ptr = builder.build_const(Type::Ptr, (uint64_t)(void*) &test_call_default_void_slot);
    Value* a = data.input(Type::Int32);
    Value* b = data.input(Type::Int32);
    Value* callee = builder.build_const(Type::Ptr, (uint64_t)(void*) test_call_default_void_store);

    builder.build_call(
      callee,
      Type::Void,
      std::vector<Value*>({out_ptr, a, b}),
      CallConv::Default
    );

    Value* observed = builder.build_load(out_ptr, Type::Int32, LoadFlags::None, AliasingGroup(0), 0);
    data.output(observed);
  });
}

int main() {
  LLVMCodeGen::initilize_llvm_jit();

  DiffTestSuite suite("tests/output/test_insts");

  test_binop(suite);
  test_shift(suite);
  test_div_mod(suite);
  test_select(suite);
  test_resize(suite);
  test_call(suite);

  return suite.finish();
}