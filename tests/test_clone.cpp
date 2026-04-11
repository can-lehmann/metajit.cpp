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

#include "../../unittest.cpp/unittest.hpp"

using namespace metajit;
using namespace metajit::test;

namespace metajit {
  namespace test {
    class CloneTest: public unittest::BaseTest<CloneTest> {
    public:
      CloneTest(const std::string& name): unittest::BaseTest<CloneTest>(name) {}

      void run(const std::function<void(Builder&)>& build) {
        unittest::BaseTest<CloneTest>::run([&]() {
          Context context;
          Allocator allocator;
          Section* section = new Section(context, allocator);

          Builder builder(section);
          build(builder);
          builder.build_exit();

          std::ostringstream original_stream;
          section->write(original_stream);

          Context cloned_context;
          Allocator cloned_allocator;
          Section* cloned_section = new Section(cloned_context, cloned_allocator);
          Clone::run(section, cloned_section);

          allocator.zero_all();
          context.const_allocator().zero_all();

          std::ostringstream cloned_stream;
          cloned_section->write(cloned_stream);

          unittest_assert(original_stream.str() == cloned_stream.str());
        });
      }
    };

    class CloneTestSuite: public unittest::Suite {
    private:
    public:
      CloneTestSuite(): unittest::Suite() {}

      CloneTest clone_test(const std::string& name) {
        return CloneTest(name).suite(*this);
      }
    };
  }
}


int main() {
  CloneTestSuite suite;

  #define binop(name) \
    suite.clone_test(#name).run([](Builder& builder) { \
      builder.move_to_end(builder.build_block({Type::Int32, Type::Int32, Type::Ptr})); \
      builder.build_store( \
        builder.entry_arg(2), \
        builder.build_##name(builder.entry_arg(0), builder.entry_arg(1)), \
        AliasingGroup(0), \
        0 \
      ); \
    });
  
  binop(add)
  binop(sub)
  binop(mul)
  binop(div_s)
  binop(div_u)
  binop(mod_s)
  binop(mod_u)
  
  binop(and)
  binop(or)
  binop(xor)

  binop(eq)
  binop(lt_u)
  binop(lt_s)

  binop(shr_u)
  binop(shr_s)
  binop(shl)

  #undef binop

  suite.clone_test("select").run([](Builder& builder) {
    builder.move_to_end(builder.build_block({Type::Bool, Type::Int32, Type::Int32, Type::Ptr}));
    builder.build_store(
      builder.entry_arg(3),
      builder.build_select(builder.entry_arg(0), builder.entry_arg(1), builder.entry_arg(2)),
      AliasingGroup(0),
      0
    );
  });

  suite.clone_test("const").run([](Builder& builder) {
    builder.move_to_end(builder.build_block({Type::Ptr}));
    builder.build_store(
      builder.entry_arg(0),
      builder.build_const(Type::Int32, 42),
      AliasingGroup(0),
      0
    );
  });

  suite.clone_test("branch").run([](Builder& builder) {
    builder.move_to_end(builder.build_block({Type::Bool, Type::Ptr}));
    Block* then_block = builder.build_block();
    Block* else_block = builder.build_block();
    builder.build_branch(builder.entry_arg(0), then_block, else_block);
    builder.move_to_end(then_block);
    builder.build_store(builder.entry_arg(1), builder.build_const(Type::Int32, 1), AliasingGroup(0), 0);
    builder.move_to_end(else_block);
    builder.build_store(builder.entry_arg(1), builder.build_const(Type::Int32, 2), AliasingGroup(0), 0);
  });

  suite.clone_test("loop").run([](Builder& builder) {
    builder.move_to_end(builder.build_block({Type::Ptr}));

    Block* loop_block = builder.build_block({Type::Int32, Type::Int32});
    Block* next_block = builder.build_block();
    Block* after_block = builder.build_block();

    builder.build_jump(loop_block, {builder.build_const(Type::Int32, 0), builder.build_const(Type::Int32, 0)});
    builder.move_to_end(loop_block);

    Value* counter = loop_block->arg(0);
    Value* sum = loop_block->arg(1);

    builder.build_branch(
      builder.build_lt_u(counter, builder.build_const(Type::Int32, 10)),
      next_block,
      after_block
    );

    builder.move_to_end(next_block);
    builder.build_jump(loop_block, {
      builder.build_add(counter, builder.build_const(Type::Int32, 1)),
      builder.build_add(sum, builder.build_const(Type::Int32, 1))
    });

    builder.move_to_end(after_block);
    builder.build_store(builder.entry_arg(0), sum, AliasingGroup(0), 0);
    builder.build_exit();
  });

  suite.clone_test("call").run([](Builder& builder) {
    builder.move_to_end(builder.build_block({Type::Ptr, Type::Int32, Type::Int32}));
    builder.build_call(
      builder.entry_arg(0),
      Type::Void,
      {builder.entry_arg(1), builder.entry_arg(2)},
      CallConv::Default
    );
  });

  return suite.finish();
}
