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

namespace metajit {
  namespace test {
    class ReentryTest: public unittest::BaseTest<ReentryTest> {
    public:
      struct TestCase {
        Inst* reentry_point = nullptr;
        std::map<Value*, Interpreter::Bits> closure;
        uint32_t expected;
      };

      ReentryTest(const std::string& name): unittest::BaseTest<ReentryTest>(name) {}

      void run(const std::function<std::vector<TestCase>(Builder&)>& build) {
        unittest::BaseTest<ReentryTest>::run([&]() {
          Context context;
          Allocator allocator;
          Section* section = new Section(context, allocator);

          Builder builder(section);
          builder.move_to_end(builder.build_block({ /* result */ Type::Ptr }));
          std::vector<TestCase> test_cases = build(builder);
          if (!builder.block()->terminator()) {
            builder.build_exit();
          }
          section->set_ordering(BlockOrdering::Topological);

          assert(!section->verify(std::cerr));

          Value* result_ptr_value = builder.entry_arg(0);

          section->autoname();

          std::set<Inst*> reentry_points;
          for (const TestCase& test_case : test_cases) {
            reentry_points.insert(test_case.reentry_point);
          }
          ReentryClosures closures(section, reentry_points);
          SliceReentryClosures::run(section, closures);

          std::ostringstream stream;
          if (section->verify(stream)) {
            stream << "\n\n";
            section->write(stream);
            throw unittest::AssertionError(
              "Failed to verify section after slicing",
              __LINE__,
              __FILE__,
              stream.str()
            );
          }

          uint8_t* closure_data = new uint8_t[closures.max_size()];
          uint32_t result = 0;

          for (const TestCase& test_case : test_cases) {
            ReentryClosures::Closure& closure = closures.at(test_case.reentry_point);
            unittest_assert(!closure.reuse);
            
            *((uint32_t*) closure_data) = closure.id;

            assert(closure.captures.size() == test_case.closure.size() + 1);

            for (ReentryClosures::Capture capture : closure.captures) {
              Interpreter::Bits value;
              if (capture.value == result_ptr_value) {
                value = Interpreter::Bits::constant(&result);
              } else {
                value = test_case.closure.at(capture.value);
              }
              value.store(closure_data + capture.offset);
            }

            Interpreter interpreter(section, {
              Interpreter::Bits::constant(closure_data)
            });

            interpreter.run();

            unittest_assert(result == test_case.expected);
          }
          
          delete[] closure_data;
          delete section;
        });
      }
    };

    class ReentryTestSuite: public unittest::Suite {
    private:
    public:
      ReentryTestSuite(int argc = 0, char** argv = nullptr): unittest::Suite(argc, argv) {}

      ReentryTest reentry_test(const std::string& name) {
        return ReentryTest(name).suite(*this);
      }
    };
  }
}

using namespace metajit;
using namespace metajit::test;

void closure_facts_seed_bridge_entry() {
  Context context;
  Allocator allocator;
  Section source(context, allocator);
  Builder builder(&source);
  Block* entry = builder.build_block({Type::Ptr, Type::Int32, Type::Ptr});
  builder.move_to_end(entry);
  builder.build_store(entry->arg(0), builder.build_const(Type::Int32, 42), 0, 8);
  Value* narrowed = builder.build_and(entry->arg(1), builder.build_const(Type::Int32, 255));
  builder.build_store(entry->arg(0), narrowed, 0, 12);
  builder.build_store(entry->arg(0), builder.build_const(Type::Int32, 7), 0, 16);
  builder.build_store(entry->arg(0), entry->arg(1), 0, 16);
  builder.build_store(entry->arg(0), entry->arg(2), 0, 24);
  Inst* end = builder.build_exit();
  OrderBlocks::run(&source, BlockOrdering::Natural);
  KnownBits bits(&source);
  BridgeFacts facts(entry, end, entry->arg(0), bits, entry->arg(2));
  unittest_assert(facts.fields.size() == 3);
  Section bridge(context, allocator);
  TraceBuilder trace(&bridge);
  Chain chain;
  trace.set_chain(&chain);
  trace.move_to_end(trace.build_block(std::vector<Type>{Type::Ptr, Type::Ptr}));
  facts.seed(trace, trace.entry_arg(0), 1, trace.entry_arg(1));
  unittest_assert(trace.build_load(trace.entry_arg(0), Type::Ptr, LoadFlags::None, 1, 24) == trace.entry_arg(1));
  Value* fixed = trace.build_load(trace.entry_arg(0), Type::Int32, LoadFlags::None, 1, 8);
  unittest_assert(dynamic_cast<Const*>(fixed) && ((Const*) fixed)->value() == 42);
  Value* value = trace.build_load(trace.entry_arg(0), Type::Int32, LoadFlags::None, 1, 12);
  unittest_assert(dynamic_cast<AssumeBitsInst*>(value));
  Value* high = trace.build_and(value, trace.build_const(Type::Int32, 0xffffff00));
  trace.build_store(trace.entry_arg(0), high, 0, 32);
  trace.build_exit();
  OrderBlocks::run(&bridge, BlockOrdering::Natural);
  Simplify::run(&bridge, 2);
  DeadCodeElim::run(&bridge);
  for (Block* block : bridge) {
    for (Inst* inst : *block) {
      if (dynmatch(StoreInst, store, inst)) {
        unittest_assert(dynamic_cast<Const*>(store->value()));
        unittest_assert(((Const*) store->value())->value() == 0);
      }
    }
  }
}

void assumed_bits_preserve_runtime_values() {
  Context context;
  Allocator allocator;
  Section section(context, allocator);
  Builder builder(&section);
  builder.move_to_end(builder.build_block({Type::Ptr}));
  Value* input = builder.build_load(builder.entry_arg(0), Type::Int32, LoadFlags::None, 0, 0);
  Value* assumed = builder.build_assume_bits(input, 0xffffff00, 0x1200);
  builder.build_store(builder.entry_arg(0), builder.build_add(assumed, builder.build_const(Type::Int32, 5)), 0, 4);
  builder.build_exit();
  OrderBlocks::run(&section, BlockOrdering::Natural);
  KnownBits bits(&section);
  unittest_assert(bits.at(assumed).mask == 0xffffff00);
  unittest_assert(bits.at(assumed).value == 0x1200);
  llvm::LLVMContext llvm_context;
  llvm::Module module("assume_bits", llvm_context);
  LLVMCodeGen llvm_codegen(&section, &module, "assume_bits");
  unittest_assert(!llvm::verifyModule(module, &llvm::errs()));
  X86CodeGen codegen(&section, {Reg::X86_R12()});
  using Function = void(* [[clang::preserve_none]])(uint32_t*);
  Function function = (Function) codegen.deploy();
  for (uint32_t low = 0; low < 256; low++) {
    uint32_t state[2] = {0x1200 + low, 0};
    function(state);
    unittest_assert(state[1] == state[0] + 5);
    state[1] = 0;
    Interpreter interpreter(&section, {Interpreter::Bits::constant(state)});
    interpreter.run();
    unittest_assert(state[1] == state[0] + 5);
  }
}

void assumed_bits_reject_conflicting_values() {
  Context context;
  Allocator allocator;
  Section section(context, allocator);
  Builder builder(&section);
  builder.move_to_end(builder.build_block({Type::Int32}));
  Value* value = builder.build_assume_bits(builder.entry_arg(0), 0xff00, 0x1200);
  builder.build_exit();
  OrderBlocks::run(&section, BlockOrdering::Natural);
  Interpreter interpreter(&section, {Interpreter::Bits::constant(Type::Int32, 0x1300)});
  interpreter.run();
  unittest_assert(interpreter.at(value).is_poison);
}

void closure_facts_stop_at_unknown_writes() {
  Context context;
  Allocator allocator;
  Section section(context, allocator);
  Builder builder(&section);
  Block* entry = builder.build_block({Type::Ptr, Type::Ptr});
  builder.move_to_end(entry);
  builder.build_store(entry->arg(0), builder.build_const(Type::Int32, 42), 0, 8);
  builder.build_store(entry->arg(1), builder.build_const(Type::Int32, 43), 0, 8);
  Inst* end = builder.build_exit();
  OrderBlocks::run(&section, BlockOrdering::Natural);
  KnownBits bits(&section);
  BridgeFacts facts(entry, end, entry->arg(0), bits);
  unittest_assert(facts.fields.empty());
}

int main(int argc, char** argv) {
  ReentryTestSuite suite(argc, argv);
  suite.test("assumed bits reject conflicting values").run(assumed_bits_reject_conflicting_values);
  suite.test("closure facts stop at unknown writes").run(closure_facts_stop_at_unknown_writes);
  suite.test("assumed bits preserve runtime values").run(assumed_bits_preserve_runtime_values);
  suite.test("closure facts seed bridge entry").run(closure_facts_seed_bridge_entry);

  suite.test("sliced continuation supports generating another guard").run([]() {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    builder.move_to_end(builder.build_block({Type::Ptr}));
    Value* value = builder.build_load(builder.entry_arg(0), Type::Int32, LoadFlags::None, 0, 0);
    Value* fixed = builder.build_assume_const(value);
    builder.build_store(builder.entry_arg(0), fixed, 0, 12);
    Value* dynamic = builder.build_load(builder.entry_arg(0), Type::Int32, LoadFlags::None, 0, 8);
    Value* promoted = builder.build_promote(dynamic);
    builder.build_store(builder.entry_arg(0), builder.build_add(promoted, builder.build_promote(fixed)), 0, 4);
    Block* yes = builder.build_block();
    Block* no = builder.build_block();
    builder.build_branch(builder.build_eq(fixed, builder.build_const(Type::Int32, 7)), yes, no);
    builder.move_to_end(yes);
    builder.build_exit();
    builder.move_to_end(no);
    builder.build_exit();
    OrderBlocks::run(&section, BlockOrdering::Topological);
    BindingTimeGroups groups(&section);
    ReentryClosures closures(&section, groups);
    SliceReentryClosures::run(&section, closures, AliasingGroup(12345), true);
    OrderBlocks::run(&section, BlockOrdering::Topological);
    Section genext(context, allocator);
    CreateGenExt::Config config;
    config.entry_arg_offset = 1;
    CreateGenExt::run(&section, &genext, config, &closures);
    std::ostringstream errors;
    unittest_assert(!genext.verify(errors));
  });

  using Bits = Interpreter::Bits;
  using TestCase = ReentryTest::TestCase;

  suite.reentry_test("add1_add1").run([](Builder& builder) {
    Value* a = builder.build_load(builder.entry_arg(0), Type::Int32, LoadFlags::None, AliasingGroup(0), 0);

    Inst* b = builder.build_add(a, builder.build_const(Type::Int32, 1));
    Inst* c = builder.build_add(b, builder.build_const(Type::Int32, 1));

    builder.build_store(builder.entry_arg(0), c, AliasingGroup(0), 0);

    return std::vector<TestCase> ({
      TestCase {
        .reentry_point = b,
        .closure = { { a, Bits::constant(Type::Int32, 41) } },
        .expected = 43
      },
      TestCase {
        .reentry_point = c,
        .closure = { { b, Bits::constant(Type::Int32, 41) } },
        .expected = 42
      }
    });
  });

  suite.reentry_test("reduce_add").run([](Builder& builder) {
    Value* a = builder.build_load(builder.entry_arg(0), Type::Int32, LoadFlags::None, AliasingGroup(0), 0);
    Value* b = builder.build_load(builder.entry_arg(0), Type::Int32, LoadFlags::None, AliasingGroup(0), 0);
    Value* c = builder.build_load(builder.entry_arg(0), Type::Int32, LoadFlags::None, AliasingGroup(0), 0);
    Value* d = builder.build_load(builder.entry_arg(0), Type::Int32, LoadFlags::None, AliasingGroup(0), 0);

    Inst* ab = builder.build_add(a, b);
    Inst* cd = builder.build_add(c, d);
    Inst* abcd = builder.build_add(ab, cd);

    builder.build_store(builder.entry_arg(0), abcd, AliasingGroup(0), 0);

    return std::vector<TestCase> ({
      TestCase {
        .reentry_point = ab,
        .closure = {
          { a, Bits::constant(Type::Int32, 10) },
          { b, Bits::constant(Type::Int32, 20) },
          { c, Bits::constant(Type::Int32, 30) },
          { d, Bits::constant(Type::Int32, 40) }
        },
        .expected = 100
      },
      TestCase {
        .reentry_point = cd,
        .closure = {
          { ab, Bits::constant(Type::Int32, 30) },
          { c, Bits::constant(Type::Int32, 30) },
          { d, Bits::constant(Type::Int32, 40) }
        },
        .expected = 100
      },
      TestCase {
        .reentry_point = abcd,
        .closure = {
          { ab, Bits::constant(Type::Int32, 30) },
          { cd, Bits::constant(Type::Int32, 70) }
        },
        .expected = 100
      }
    });
  });

  suite.reentry_test("reduce_add_only_cd").run([](Builder& builder) {
    Value* a = builder.build_load(builder.entry_arg(0), Type::Int32, LoadFlags::None, AliasingGroup(0), 0);
    Value* b = builder.build_load(builder.entry_arg(0), Type::Int32, LoadFlags::None, AliasingGroup(0), 0);
    Value* c = builder.build_load(builder.entry_arg(0), Type::Int32, LoadFlags::None, AliasingGroup(0), 0);
    Value* d = builder.build_load(builder.entry_arg(0), Type::Int32, LoadFlags::None, AliasingGroup(0), 0);

    Inst* ab = builder.build_add(a, b);
    Inst* cd = builder.build_add(c, d);
    Inst* abcd = builder.build_add(ab, cd);

    builder.build_store(builder.entry_arg(0), abcd, AliasingGroup(0), 0);

    return std::vector<TestCase> ({
      TestCase {
        .reentry_point = cd,
        .closure = {
          { ab, Bits::constant(Type::Int32, 30) },
          { c, Bits::constant(Type::Int32, 30) },
          { d, Bits::constant(Type::Int32, 40) }
        },
        .expected = 100
      }
    });
  });

  suite.reentry_test("branch").run([](Builder& builder) {
    Value* cond = builder.build_load(builder.entry_arg(0), Type::Bool, LoadFlags::None, AliasingGroup(0), 0);

    Block* true_block = builder.build_block();
    Block* false_block = builder.build_block();

    Inst* branch = builder.build_branch(cond, true_block, false_block);

    builder.move_to_end(true_block);
    Inst* store_true = builder.build_store(builder.entry_arg(0), builder.build_const(Type::Int32, 1), AliasingGroup(0), 0);
    builder.build_exit();

    builder.move_to_end(false_block);
    Inst* store_false = builder.build_store(builder.entry_arg(0), builder.build_const(Type::Int32, 2), AliasingGroup(0), 0);
    builder.build_exit();

    return std::vector<TestCase> ({
      TestCase {
        .reentry_point = branch,
        .closure = { { cond, Bits::constant(true) } },
        .expected = 1
      },
      TestCase {
        .reentry_point = branch,
        .closure = { { cond, Bits::constant(false) } },
        .expected = 2
      },
      TestCase {
        .reentry_point = store_true,
        .closure = { },
        .expected = 1
      },
      TestCase {
        .reentry_point = store_false,
        .closure = { },
        .expected = 2
      }
    });
  });

  suite.reentry_test("branch_continue").run([](Builder& builder) {
    Value* cond = builder.build_load(builder.entry_arg(0), Type::Bool, LoadFlags::None, AliasingGroup(0), 0);

    Block* true_block = builder.build_block();
    Block* false_block = builder.build_block();
    Block* cont_block = builder.build_block();

    Inst* branch = builder.build_branch(cond, true_block, false_block);

    builder.move_to_end(true_block);
    Inst* store_true = builder.build_store(builder.entry_arg(0), builder.build_const(Type::Int32, 1), AliasingGroup(0), 0);
    builder.build_jump(cont_block);

    builder.move_to_end(false_block);
    Inst* store_false = builder.build_store(builder.entry_arg(0), builder.build_const(Type::Int32, 2), AliasingGroup(0), 0);
    builder.build_jump(cont_block);

    builder.move_to_end(cont_block);
    builder.build_exit();

    return std::vector<TestCase> ({
      TestCase {
        .reentry_point = branch,
        .closure = { { cond, Bits::constant(true) } },
        .expected = 1
      },
      TestCase {
        .reentry_point = branch,
        .closure = { { cond, Bits::constant(false) } },
        .expected = 2
      }
    });
  });

  suite.reentry_test("branch_join").run([](Builder& builder) {
    Value* cond = builder.build_load(builder.entry_arg(0), Type::Bool, LoadFlags::None, AliasingGroup(0), 0);
    Value* value = builder.build_load(builder.entry_arg(0), Type::Int32, LoadFlags::None, AliasingGroup(0), 0);

    Block* true_block = builder.build_block();
    Block* false_block = builder.build_block();
    Block* cont_block = builder.build_block();

    Inst* branch = builder.build_branch(cond, true_block, false_block);

    builder.move_to_end(true_block);
    Inst* jump_true = builder.build_jump(cont_block);

    builder.move_to_end(false_block);
    Inst* jump_false = builder.build_jump(cont_block);

    builder.move_to_end(cont_block);
    builder.build_store(builder.entry_arg(0), value, AliasingGroup(0), 0);
    builder.build_exit();

    return std::vector<TestCase> ({
      TestCase {
        .reentry_point = jump_true,
        .closure = {
          { value, Bits::constant(Type::Int32, 42) }
        },
        .expected = 42
      },
      TestCase {
        .reentry_point = jump_false,
        .closure = {
          { value, Bits::constant(Type::Int32, 123) }
        },
        .expected = 123
      }
    });
  });

  suite.reentry_test("branch_join_nested").run([](Builder& builder) {
    Value* cond1 = builder.build_load(builder.entry_arg(0), Type::Bool, LoadFlags::None, AliasingGroup(0), 0);
    Value* cond2 = builder.build_load(builder.entry_arg(0), Type::Bool, LoadFlags::None, AliasingGroup(0), 0);
    Value* value = builder.build_load(builder.entry_arg(0), Type::Int32, LoadFlags::None, AliasingGroup(0), 0);

    Block* true_block_outer = builder.build_block();
    Block* true_block_inner = builder.build_block();
    Block* false_block_inner = builder.build_block();
    Block* cont_block_inner = builder.build_block();
    Block* false_block_outer = builder.build_block();
    Block* cont_block_outer = builder.build_block();

    Inst* branch_outer = builder.build_branch(cond1, true_block_outer, false_block_outer);

    builder.move_to_end(true_block_outer);
    Inst* branch_inner = builder.build_branch(cond2, true_block_inner, false_block_inner);

    builder.move_to_end(true_block_inner);
    Inst* jump_true_inner = builder.build_jump(cont_block_inner);

    builder.move_to_end(false_block_inner);
    Inst* jump_false_inner = builder.build_jump(cont_block_inner);

    builder.move_to_end(cont_block_inner);
    builder.build_jump(cont_block_outer);

    builder.move_to_end(false_block_outer);
    builder.build_jump(cont_block_outer);

    builder.move_to_end(cont_block_outer);
    builder.build_store(builder.entry_arg(0), value, AliasingGroup(0), 0);
    builder.build_exit();

    return std::vector<TestCase> ({
      TestCase {
        .reentry_point = jump_true_inner,
        .closure = {
          { value, Bits::constant(Type::Int32, 42) }
        },
        .expected = 42
      },
      TestCase {
        .reentry_point = jump_false_inner,
        .closure = {
          { value, Bits::constant(Type::Int32, 123) }
        },
        .expected = 123
      }
    });
  });

  suite.reentry_test("branch_join_arg").run([](Builder& builder) {
    Value* cond = builder.build_load(builder.entry_arg(0), Type::Bool, LoadFlags::None, AliasingGroup(0), 0);
    Value* value = builder.build_load(builder.entry_arg(0), Type::Int32, LoadFlags::None, AliasingGroup(0), 0);

    Block* true_block = builder.build_block();
    Block* false_block = builder.build_block();
    Block* cont_block = builder.build_block({Type::Int32});

    Inst* branch = builder.build_branch(cond, true_block, false_block);

    builder.move_to_end(true_block);
    Inst* jump_true = builder.build_jump(cont_block, { builder.build_const(Type::Int32, 1) });

    builder.move_to_end(false_block);
    Inst* jump_false = builder.build_jump(cont_block, { builder.build_const(Type::Int32, 2) });

    builder.move_to_end(cont_block);
    Value* add = builder.build_add(cont_block->arg(0), value);
    builder.build_store(builder.entry_arg(0), add, AliasingGroup(0), 0);
    builder.build_exit();

    return std::vector<TestCase> ({
      TestCase {
        .reentry_point = jump_true,
        .closure = {
          { value, Bits::constant(Type::Int32, 42) }
        },
        .expected = 43
      },
      TestCase {
        .reentry_point = jump_false,
        .closure = {
          { value, Bits::constant(Type::Int32, 123) }
        },
        .expected = 125
      }
    });
  });

  suite.reentry_test("branch_skip").run([](Builder& builder) {
    Value* cond = builder.build_load(builder.entry_arg(0), Type::Bool, LoadFlags::None, AliasingGroup(0), 0);
    Value* value = builder.build_load(builder.entry_arg(0), Type::Int32, LoadFlags::None, AliasingGroup(0), 0);

    Block* true_block = builder.build_block();
    Block* cont_block = builder.build_block();

    Inst* branch = builder.build_branch(cond, true_block, cont_block);
    
    builder.move_to_end(true_block);
    Inst* jump_true = builder.build_jump(cont_block);

    builder.move_to_end(cont_block);
    builder.build_store(builder.entry_arg(0), value, AliasingGroup(0), 0);
    builder.build_exit();

    return std::vector<TestCase> ({
      TestCase {
        .reentry_point = jump_true,
        .closure = {
          { value, Bits::constant(Type::Int32, 42) }
        },
        .expected = 42
      },
      TestCase {
        .reentry_point = branch,
        .closure = {
          { cond, Bits::constant(false) },
          { value, Bits::constant(Type::Int32, 123) }
        },
        .expected = 123
      }
    });
  });

  return suite.finish();
}
