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
          builder.build_exit();
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
      ReentryTestSuite(): unittest::Suite() {}

      ReentryTest reentry_test(const std::string& name) {
        return ReentryTest(name).suite(*this);
      }
    };
  }
}

using namespace metajit;
using namespace metajit::test;

int main() {
  ReentryTestSuite suite;

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

  return suite.finish();
}
