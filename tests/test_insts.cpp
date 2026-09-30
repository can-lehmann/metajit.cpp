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

  binop(and, true)
  binop(or, true)
  binop(xor, true)

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

// Takes two separate pointer args: writes sum of their first int32s to out[0]
extern "C" __attribute__((noinline))
void test_call_two_ptr_args(uint32_t* out, uint32_t* a, uint32_t* b) {
  out[0] = *a + *b;
}

void test_freeze(DiffTestSuite& suite) {
  #define freeze_type(type) \
    suite.diff_test("freeze_" #type).run([](Builder& builder, TestData& data) { \
      data.output(builder.build_freeze(data.input(Type::type))); \
    });

  freeze_type(Bool)
  freeze_type(Int8)
  freeze_type(Int16)
  freeze_type(Int32)
  freeze_type(Int64)

  // freeze(poison) - freeze(poison) == 0: freeze returns a fixed value, not a fresh one each use
  suite.diff_test("freeze_poison_shl_Int32").run([](Builder& builder, TestData& data) {
    Value* val = data.input(Type::Int32);
    Value* shift = data.input(RandomRange(Type::Int32, 32, type_mask(Type::Int32)));
    Value* frozen = builder.build_freeze(builder.build_shl(val, shift));
    data.output(builder.build_sub(frozen, frozen));
  });
}

void test_assume_const(DiffTestSuite& suite) {
  #define assume_const_type(type) \
    suite.diff_test("assume_const_" #type).run([](Builder& builder, TestData& data) { \
      data.output(builder.build_assume_const(data.input(Type::type))); \
    });

  assume_const_type(Bool)
  assume_const_type(Int8)
  assume_const_type(Int16)
  assume_const_type(Int32)
  assume_const_type(Int64)
}

void test_alloca(DiffTestSuite& suite) {
  suite.diff_test("alloca_store_load_Int32").run([](Builder& builder, TestData& data) {
    Value* ptr = builder.build_alloca(builder.build_const(Type::Int64, 4), 4);
    Value* val = data.input(Type::Int32);
    builder.build_store(ptr, val, AliasingGroup(0), 0);
    Value* loaded = builder.build_load(ptr, Type::Int32, LoadFlags::None, AliasingGroup(0), 0);
    data.output(loaded);
  });

  suite.diff_test("alloca_store_load_Int64").run([](Builder& builder, TestData& data) {
    Value* ptr = builder.build_alloca(builder.build_const(Type::Int64, 8), 8);
    Value* val = data.input(Type::Int64);
    builder.build_store(ptr, val, AliasingGroup(0), 0);
    Value* loaded = builder.build_load(ptr, Type::Int64, LoadFlags::None, AliasingGroup(0), 0);
    data.output(loaded);
  });

  suite.diff_test("alloca_multiple_stores").run([](Builder& builder, TestData& data) {
    Value* ptr = builder.build_alloca(builder.build_const(Type::Int64, 8), 8);
    Value* val1 = data.input(Type::Int32);
    Value* val2 = data.input(Type::Int32);
    builder.build_store(ptr, val1, AliasingGroup(0), 0);
    builder.build_store(ptr, val2, AliasingGroup(0), 0);
    Value* loaded = builder.build_load(ptr, Type::Int32, LoadFlags::None, AliasingGroup(0), 0);
    data.output(loaded);
  });

  suite.diff_test("alloca_add_ptr_store_load_Int32").run([](Builder& builder, TestData& data) {
    Value* ptr = builder.build_alloca(builder.build_const(Type::Int64, 16), 8);
    Value* ptr_off = builder.build_add_ptr(ptr, builder.build_const(Type::Int64, 4));
    Value* val = data.input(Type::Int32);
    builder.build_store(ptr_off, val, AliasingGroup(0), 0);
    Value* loaded = builder.build_load(ptr_off, Type::Int32, LoadFlags::None, AliasingGroup(0), 0);
    data.output(loaded);
  });
}

void test_load_store_f(DiffTestSuite& suite) {
  for (Type type : {Type::Float32, Type::Float64}) {
    Type bits_type = type == Type::Float32 ? Type::Int32 : Type::Int64;
    std::string name = std::string("load_store_") + to_string(type);

    suite.diff_test(name + "_roundtrip").run([=](Builder& builder, TestData& data) {
      Value* ptr = builder.build_alloca(builder.build_const(Type::Int64, 24), 8);
      Value* sentinel = builder.build_const(Type::Int64, 0x123456789abcdef0ULL);
      for (int32_t offset : {0, 8, 16}) {
        builder.build_store(ptr, sentinel, AliasingGroup(0), offset);
      }
      Value* first = data.input(type);
      Value* second = data.input(type);
      builder.build_store(ptr, first, AliasingGroup(0), 8);
      data.output(builder.build_load(ptr, type, LoadFlags::None, AliasingGroup(0), 8));
      data.output(builder.build_load(ptr, Type::Int64, LoadFlags::None, AliasingGroup(0), 8));
      builder.build_store(ptr, second, AliasingGroup(0), 8);
      data.output(builder.build_load(ptr, type, LoadFlags::None, AliasingGroup(0), 8));
      for (int32_t offset : {0, 8, 16}) {
        data.output(builder.build_load(ptr, Type::Int64, LoadFlags::None, AliasingGroup(0), offset));
      }
    });

    suite.diff_test(name + "_offsets").run([=](Builder& builder, TestData& data) {
      Value* ptr = builder.build_alloca(builder.build_const(Type::Int64, 512), 8);
      for (int32_t offset : {1, 127, 257}) {
        Value* value = data.input(type);
        builder.build_store(ptr, value, AliasingGroup(0), offset);
        Value* shifted = builder.build_add_ptr(ptr, builder.build_const(Type::Int64, offset + 3));
        data.output(builder.build_load(shifted, type, LoadFlags::None, AliasingGroup(0), -3));
        data.output(builder.build_load(ptr, bits_type, LoadFlags::None, AliasingGroup(0), offset));
      }
    });

    suite.diff_test(name + "_constants").run([=](Builder& builder, TestData& data) {
      Value* ptr = builder.build_alloca(builder.build_const(Type::Int64, 8), 8);
      for (uint64_t bits : {uint64_t(0), uint64_t(1), uint64_t(1) << (type_size(type) * 8 - 1), type_mask(type)}) {
        builder.build_store(ptr, builder.build_const(type, bits), AliasingGroup(0), 0);
        data.output(builder.build_load(ptr, type, LoadFlags::None, AliasingGroup(0), 0));
        data.output(builder.build_load(ptr, bits_type, LoadFlags::None, AliasingGroup(0), 0));
      }
    });

    suite.diff_test(name + "_pressure").aot(false).run([=](Builder& builder, TestData& data) {
      Value* ptr = builder.build_alloca(builder.build_const(Type::Int64, 24 * 8), 8);
      std::vector<Value*> values;
      for (int32_t index = 0; index < 24; index++) {
        values.push_back(data.input(type));
      }
      for (int32_t index = 0; index < 24; index++) {
        builder.build_store(ptr, values[index], AliasingGroup(0), index * 8);
      }
      for (int32_t index = 0; index < 24; index++) {
        data.output(values[index]);
        data.output(builder.build_load(ptr, type, LoadFlags::None, AliasingGroup(0), index * 8));
        data.output(builder.build_load(ptr, bits_type, LoadFlags::None, AliasingGroup(0), index * 8));
      }
    });
  }
}

void test_call(DiffTestSuite& suite) {
  suite.diff_test("call_preserve_none").aot(false).interpreter(false).run([](Builder& builder, TestData& data) {
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

  suite.diff_test("call_preserve_none_0").aot(false).interpreter(false).run([](Builder& builder, TestData& data) {
    Value* callee = builder.build_const(Type::Ptr, (uint64_t)(void*) test_call_preserve_none_target_0);
    Value* result = builder.build_call(callee, Type::Int64, std::vector<Value*>(), CallConv::PreserveNone);
    data.output(result);
  });

  suite.diff_test("call_preserve_none_1_smoke").aot(false).interpreter(false).run([](Builder& builder, TestData& data) {
    Value* a = data.input(Type::Int64);

    Value* callee = builder.build_const(Type::Ptr, (uint64_t)(void*) test_call_preserve_none_target_1);
    builder.build_call(
      callee,
      Type::Int64,
      std::vector<Value*>({a}),
      CallConv::PreserveNone
    );
  });

  suite.diff_test("call_preserve_none_4_smoke").aot(false).interpreter(false).run([](Builder& builder, TestData& data) {
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

  suite.diff_test("call_default").aot(false).interpreter(false).run([](Builder& builder, TestData& data) {
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

  suite.diff_test("call_default_0").aot(false).interpreter(false).run([](Builder& builder, TestData& data) {
    Value* callee = builder.build_const(Type::Ptr, (uint64_t)(void*) test_call_default_target_0);
    Value* result = builder.build_call(callee, Type::Int64, std::vector<Value*>(), CallConv::Default);
    data.output(result);
  });

  suite.diff_test("call_default_1").aot(false).interpreter(false).run([](Builder& builder, TestData& data) {
    Value* a = data.input(Type::Int64);
    Value* callee = builder.build_const(Type::Ptr, (uint64_t)(void*) test_call_default_target_1);
    Value* result = builder.build_call(callee, Type::Int64, std::vector<Value*>({a}), CallConv::Default);
    data.output(result);
  });

  suite.diff_test("call_default_2").aot(false).interpreter(false).run([](Builder& builder, TestData& data) {
    Value* a = data.input(Type::Int64);
    Value* b = data.input(Type::Int64);
    Value* callee = builder.build_const(Type::Ptr, (uint64_t)(void*) test_call_default_target_2);
    Value* result = builder.build_call(callee, Type::Int64, std::vector<Value*>({a, b}), CallConv::Default);
    data.output(result);
  });

  suite.diff_test("call_default_4").aot(false).interpreter(false).run([](Builder& builder, TestData& data) {
    Value* a = data.input(Type::Int64);
    Value* b = data.input(Type::Int64);
    Value* c = data.input(Type::Int64);
    Value* d = data.input(Type::Int64);
    Value* callee = builder.build_const(Type::Ptr, (uint64_t)(void*) test_call_default_target_4);
    Value* result = builder.build_call(callee, Type::Int64, std::vector<Value*>({a, b, c, d}), CallConv::Default);
    data.output(result);
  });

  suite.diff_test("call_default_mixed_types").aot(false).interpreter(false).run([](Builder& builder, TestData& data) {
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

  suite.diff_test("call_default_ptr_ret").aot(false).interpreter(false).run([](Builder& builder, TestData& data) {
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

  // Two consecutive calls where the second call takes args from the results/inputs
  // of the first — exercises the parallel-move problem in call argument setup.
  suite.diff_test("call_default_two_calls_two_args").aot(false).interpreter(false).run([](Builder& builder, TestData& data) {
    Value* a = data.input(Type::Int64);
    Value* b = data.input(Type::Int64);
    Value* callee2 = builder.build_const(Type::Ptr, (uint64_t)(void*) test_call_default_target_2);
    Value* r1 = builder.build_call(callee2, Type::Int64, std::vector<Value*>({a, b}), CallConv::Default);
    // Second call: pass r1 and a — a was live across the first call
    Value* r2 = builder.build_call(callee2, Type::Int64, std::vector<Value*>({r1, a}), CallConv::Default);
    data.output(r2);
  });

  // Section with two entry-like ptr args (simulate genext scenario):
  // call a function passing both ptr args as arguments.
  suite.diff_test("call_default_two_ptr_entry_args").aot(false).interpreter(false).run([](Builder& builder, TestData& data) {
    Value* a = data.input(Type::Int64);
    Value* b = data.input(Type::Int64);
    Value* c = data.input(Type::Int64);
    Value* callee3 = builder.build_const(Type::Ptr, (uint64_t)(void*) test_call_default_target);
    // First call uses a and b, second uses b and c — b must survive the first call
    Value* r1 = builder.build_call(callee3, Type::Int64, std::vector<Value*>({a, b, c}), CallConv::Default);
    Value* r2 = builder.build_call(callee3, Type::Int64, std::vector<Value*>({b, c, r1}), CallConv::Default);
    data.output(r2);
  });

  suite.diff_test("call_default_void_ret").aot(false).interpreter(false).run([](Builder& builder, TestData& data) {
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

#define fp_call_targets(prefix, attrs) \
  template <typename F> attrs F prefix##_identity(F value) { return value; } \
  template <typename F> attrs F prefix##_zero() { return (F) -3.25; } \
  template <typename F> attrs uint64_t prefix##_integer(F value, uint64_t integer) { \
    return (uint64_t) value + integer; \
  } \
  template <typename F> attrs void prefix##_void(F value, F* out) { *out = value; } \
  template <typename F> attrs F prefix##_mixed( \
      uint64_t i0, F f0, uint64_t i1, double f1, uint64_t i2, float f2, \
      uint64_t i3, F f3, uint64_t i4, F f4, uint64_t i5, F f5, F f6, F f7) { \
    return (F) (i0 + 2 * i1 + 3 * i2 + 4 * i3 + 5 * i4 + 6 * i5) + \
           f0 + (F) (2 * f1) + (F) (3 * f2) + 4 * f3 + 5 * f4 + 6 * f5 + 7 * f6 + 8 * f7; \
  }

fp_call_targets(test_call_fp_default, __attribute__((noinline)))
fp_call_targets(test_call_fp_preserve_none, __attribute__((preserve_none, noinline)))

#undef fp_call_targets

template <typename F>
void test_call_fp(DiffTestSuite& suite, Type type) {
  for (CallConv call_conv : {CallConv::Default, CallConv::PreserveNone}) {
    std::string name = std::string("call_fp_") + to_string(type) +
                       (call_conv == CallConv::Default ? "_default" : "_preserve_none");
    uint64_t identity = call_conv == CallConv::Default ?
      (uint64_t)(void*) test_call_fp_default_identity<F> : (uint64_t)(void*) test_call_fp_preserve_none_identity<F>;
    uint64_t zero = call_conv == CallConv::Default ?
      (uint64_t)(void*) test_call_fp_default_zero<F> : (uint64_t)(void*) test_call_fp_preserve_none_zero<F>;
    uint64_t integer = call_conv == CallConv::Default ?
      (uint64_t)(void*) test_call_fp_default_integer<F> : (uint64_t)(void*) test_call_fp_preserve_none_integer<F>;
    uint64_t void_target = call_conv == CallConv::Default ?
      (uint64_t)(void*) test_call_fp_default_void<F> : (uint64_t)(void*) test_call_fp_preserve_none_void<F>;
    uint64_t mixed = call_conv == CallConv::Default ?
      (uint64_t)(void*) test_call_fp_default_mixed<F> : (uint64_t)(void*) test_call_fp_preserve_none_mixed<F>;

    suite.diff_test(name + "_identity").aot(false).interpreter(false).run([=](Builder& builder, TestData& data) {
      Value* value = data.input(type);
      Value* callee = builder.build_const(Type::Ptr, identity);
      Value* first = builder.build_call(callee, type, {value}, call_conv);
      Value* second = builder.build_call(callee, type, {first}, call_conv);
      data.output(value);
      data.output(first);
      data.output(second);
    });

    suite.diff_test(name + "_zero_args").aot(false).interpreter(false).run([=](Builder& builder, TestData& data) {
      data.output(builder.build_call(builder.build_const(Type::Ptr, zero), type, std::vector<Value*>(), call_conv));
    });

    suite.diff_test(name + "_constant").aot(false).interpreter(false).run([=](Builder& builder, TestData& data) {
      Value* value = builder.build_int_to_float_s(builder.build_const(Type::Int64, 17), type);
      data.output(builder.build_call(builder.build_const(Type::Ptr, identity), type, {value}, call_conv));
    });

    suite.diff_test(name + "_special_values").aot(false).interpreter(false).run([=](Builder& builder, TestData& data) {
      std::vector<uint64_t> values = type == Type::Float32 ?
        std::vector<uint64_t>{0, 0x80000000, 0x7f800000, 0xff800000, 0x7fc00001, 1} :
        std::vector<uint64_t>{0, 0x8000000000000000ULL, 0x7ff0000000000000ULL,
                             0xfff0000000000000ULL, 0x7ff8000000000001ULL, 1};
      Value* callee = builder.build_const(Type::Ptr, identity);
      for (uint64_t bits : values) {
        data.output(builder.build_call(callee, type, {builder.build_const(type, bits)}, call_conv));
      }
    });

    suite.diff_test(name + "_integer_return").aot(false).interpreter(false).run([=](Builder& builder, TestData& data) {
      Value* value = builder.build_int_to_float_s(data.input(RandomRange(Type::Int64, 0, 100)), type);
      Value* i = data.input(Type::Int64);
      data.output(builder.build_call(builder.build_const(Type::Ptr, integer), Type::Int64, {value, i}, call_conv));
      data.output(value);
      data.output(i);
    });

    suite.diff_test(name + "_void_return").aot(false).interpreter(false).run([=](Builder& builder, TestData& data) {
      Value* value = data.input(type);
      Value* out = builder.build_alloca(builder.build_const(Type::Int64, 8), 8);
      builder.build_call(builder.build_const(Type::Ptr, void_target), Type::Void, {value, out}, call_conv);
      data.output(builder.build_load(out, type, LoadFlags::None, AliasingGroup(0), 0));
      data.output(value);
    });

    suite.diff_test(name + "_mixed_pressure").aot(false).interpreter(false).run([=](Builder& builder, TestData& data) {
      std::vector<Value*> floats;
      std::vector<Value*> integers;
      for (size_t index = 0; index < 24; index++) {
        floats.push_back(builder.build_int_to_float_s(data.input(RandomRange(Type::Int64, 1, 32)), type));
      }
      for (size_t index = 0; index < 16; index++) {
        integers.push_back(data.input(RandomRange(Type::Int64, 1, 32)));
      }
      std::vector<Value*> args;
      for (size_t index = 0; index < 6; index++) {
        args.push_back(integers[index]);
        Type arg_type = index == 1 ? Type::Float64 : index == 2 ? Type::Float32 : type;
        args.push_back(arg_type == type ? floats[index] : builder.build_resize_f(floats[index], arg_type));
      }
      args.push_back(floats[6]);
      args.push_back(floats[7]);
      Value* callee = builder.build_const(Type::Ptr, mixed);
      Value* first = builder.build_call(callee, type, args, call_conv);
      args[1] = first;
      args[13] = first;
      Value* second = builder.build_call(callee, type, args, call_conv);
      data.output(first);
      data.output(second);
      for (Value* value : floats) {
        data.output(value);
      }
      for (Value* value : integers) {
        data.output(value);
      }
    });
  }
}

uint64_t test_call_clobber_xmm() {
  asm volatile("xorps %%xmm0, %%xmm0\n\txorps %%xmm15, %%xmm15" ::: "xmm0", "xmm15");
  return 42;
}

void test_binop_f(DiffTestSuite& suite) {
  for (Type type : {Type::Float32, Type::Float64}) {
    suite.diff_test(std::string("float_spills_") + to_string(type)).aot(false).run([type](Builder& builder, TestData& data) {
      std::vector<Value*> values;
      for (size_t index = 0; index < 24; index++) {
        values.push_back(data.input(type));
      }
      Value* integer = data.input(Type::Int64);
      for (Value* value : values) {
        data.output(builder.build_add_f(value, value));
      }
      data.output(integer);
    });

    suite.diff_test(std::string("float_across_call_") + to_string(type)).aot(false).interpreter(false).run([type](Builder& builder, TestData& data) {
      std::vector<Value*> values;
      for (size_t index = 0; index < 16; index++) {
        values.push_back(data.input(type));
      }
      Value* callee = builder.build_const(Type::Ptr, (uint64_t)(void*) test_call_clobber_xmm);
      data.output(builder.build_call(callee, Type::Int64, std::vector<Value*>(), CallConv::Default));
      for (Value* value : values) {
        data.output(value);
      }
    });

    suite.diff_test(std::string("mixed_register_classes_") + to_string(type)).run([type](Builder& builder, TestData& data) {
      std::vector<Value*> floats;
      std::vector<Value*> integers;
      for (size_t index = 0; index < 10; index++) {
        floats.push_back(data.input(type));
        integers.push_back(data.input(Type::Int64));
      }
      for (size_t index = 0; index < floats.size(); index++) {
        data.output(builder.build_add_f(floats[index], floats[index]));
        data.output(builder.build_lt_f_o(floats[index], floats[0]));
        data.output(builder.build_add(integers[index], integers[index]));
      }
    });
  }

  #define binop_f_type(name, type) \
    suite.diff_test(#name "_" #type).run([](Builder& builder, TestData& data) { \
      data.output(builder.build_##name(data.input(Type::type), data.input(Type::type))); \
    }); \
    suite.diff_test(#name "_" #type "_imm").run([](Builder& builder, TestData& data) { \
      data.output(builder.build_##name(data.input(Type::type), RandomRange(Type::type).gen_const(builder))); \
    });
  
  #define binop_f(name) \
    binop_f_type(name, Float32) \
    binop_f_type(name, Float64)
  
  binop_f(add_f)
  binop_f(sub_f)
  binop_f(mul_f)
  binop_f(div_f)
  
  binop_f(lt_f_u)
  binop_f(lt_f_o)
}

void test_convert_f(DiffTestSuite& suite) {
  #define resize_f_type(from_type, to_type) \
    suite.diff_test("resize_f_" #from_type "_to_" #to_type).run([](Builder& builder, TestData& data) { \
      data.output(builder.build_resize_f(data.input(Type::from_type), Type::to_type)); \
    });

  resize_f_type(Float32, Float64)
  resize_f_type(Float64, Float32)

  #undef resize_f_type

  #define int_to_float_s_type(from_type, to_type) \
    suite.diff_test("int_to_float_s_" #from_type "_to_" #to_type).run([](Builder& builder, TestData& data) { \
      data.output(builder.build_int_to_float_s(data.input(Type::from_type), Type::to_type)); \
    });

  int_to_float_s_type(Int8, Float32)
  int_to_float_s_type(Int8, Float64)
  int_to_float_s_type(Int16, Float32)
  int_to_float_s_type(Int16, Float64)
  int_to_float_s_type(Int32, Float32)
  int_to_float_s_type(Int32, Float64)
  int_to_float_s_type(Int64, Float32)
  int_to_float_s_type(Int64, Float64)

  #undef int_to_float_s_type

  #define float_to_int_s_type(from_type, to_type, max_safe) \
    suite.diff_test("float_to_int_s_" #from_type "_to_" #to_type).run([](Builder& builder, TestData& data) { \
      Value* i = data.input(RandomRange(Type::to_type, 0, max_safe)); \
      Value* f = builder.build_int_to_float_s(i, Type::from_type); \
      data.output(builder.build_float_to_int_s(f, Type::to_type)); \
    });

  float_to_int_s_type(Float32, Int8, 100)
  float_to_int_s_type(Float32, Int16, 20000)
  float_to_int_s_type(Float32, Int32, 1000000000)
  float_to_int_s_type(Float32, Int64, 1000000000)
  float_to_int_s_type(Float64, Int8, 100)
  float_to_int_s_type(Float64, Int16, 20000)
  float_to_int_s_type(Float64, Int32, 1000000000)
  float_to_int_s_type(Float64, Int64, 1000000000000000000ULL)

  #undef float_to_int_s_type
}

template<Type result_type>
void ptr_to_int_outputs(Builder& builder, TestData& data, RandomRange range) {
  Value* result = builder.build_ptr_to_int(data.input(range), result_type);
  data.output(result);
  if (result_type != Type::Int64) {
    data.output(builder.build_resize_u(result, Type::Int64));
  }
}

template<Type result_type>
void ptr_to_int_inputs(Builder& builder, TestData& data) {
  ptr_to_int_outputs<result_type>(builder, data, RandomRange(Type::Ptr));
  for (uint64_t bits : {0ULL, 0x80ULL, 0x8000ULL, 0x80000000ULL,
                        0x8000000000000000ULL, 0xffffffffffffffffULL,
                        0xfedcba9876543280ULL}) {
    ptr_to_int_outputs<result_type>(builder, data, RandomRange(Type::Ptr, bits, bits));
  }
}

void test_ptr_to_int(DiffTestSuite& suite) {
  suite.diff_test("ptr_to_int_Int8").run(ptr_to_int_inputs<Type::Int8>);
  suite.diff_test("ptr_to_int_Int16").run(ptr_to_int_inputs<Type::Int16>);
  suite.diff_test("ptr_to_int_Int32").run(ptr_to_int_inputs<Type::Int32>);
  suite.diff_test("ptr_to_int_Int64").run(ptr_to_int_inputs<Type::Int64>);
}

void test_popcount(DiffTestSuite& suite) {
  #define popcount_type(type) \
    suite.diff_test("popcount_" #type).run([](Builder& builder, TestData& data) { \
      data.output(builder.build_popcount(data.input(Type::type))); \
    });

  popcount_type(Int8)
  popcount_type(Int16)
  popcount_type(Int32)
  popcount_type(Int64)
}

int main(int argc, char** argv) {
  LLVMCodeGen::initilize_llvm_jit();

  DiffTestSuite suite("tests/output/test_insts", argc, argv);

  test_binop(suite);
  test_shift(suite);
  test_div_mod(suite);
  test_select(suite);
  test_resize(suite);
  test_freeze(suite);
  test_assume_const(suite);
  test_alloca(suite);
  test_load_store_f(suite);
  test_call(suite);
  test_call_fp<float>(suite, Type::Float32);
  test_call_fp<double>(suite, Type::Float64);
  test_binop_f(suite);
  test_convert_f(suite);
  test_ptr_to_int(suite);
  test_popcount(suite);

  return suite.finish();
}
