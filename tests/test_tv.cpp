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

namespace metajit {
  namespace test {
    class TVTest: public unittest::BaseTest<TVTest> {
    public:
      TVTest(const std::string& name): unittest::BaseTest<TVTest>(name) {}

      void run(std::vector<Type> entry_args,
               std::function<Value*(Builder&)> build,
               std::function<z3::expr(z3::context&, std::vector<tv::ValueState>)> expected) {
        
        unittest::BaseTest<TVTest>::run([&]() {
          Context context;
          Allocator allocator;
          Section* section = new Section(context, allocator);

          Builder builder(section);
          builder.move_to_end(builder.build_block(entry_args));
          Value* result = build(builder);
          builder.build_exit();

          section->autoname();
          
          z3::context z3_context;
          std::vector<tv::ValueState> z3_entry_args;
          for (size_t it = 0; it < entry_args.size(); it++) {
            z3_entry_args.push_back(tv::ValueState(
              entry_args[it],
              z3_context.bv_const(
                ("arg" + std::to_string(it)).c_str(),
                type_width(entry_args[it])
              )
            ));
          }

          tv::MemoryState memory_state(z3_context, {});
          tv::Z3CodeGen codegen(section, z3_context, z3_entry_args, memory_state);

          z3::solver solver(z3_context);

          z3::expr tv_result = codegen.emit(result).value();
          z3::expr expected_result = expected(z3_context, z3_entry_args);

          solver.add((tv_result != expected_result).simplify());
          
          z3::check_result check_result = solver.check();
          if (check_result == z3::sat) {
            z3::model model = solver.get_model();
            std::ostringstream stream;
            stream << "Arguments:\n";
            for (Arg* arg : section->entry()->args()) {
              tv::ValueState state = codegen.emit(arg).eval(model);
              stream << arg->name() << " = " << state.value() << "\n";
            }

            stream << "\n";
            
            stream << "TV: " << model.eval(tv_result, true) << "\n";
            stream << "Expected: " << model.eval(expected_result, true) << "\n";

            throw unittest::AssertionError(
              "Counterexample found",
              __LINE__,
              __FILE__,
              stream.str()
            );
          } else if (check_result != z3::unsat) {
            throw unittest::AssertionError("Failed to prove", __LINE__, __FILE__);
          }
        });
      }
    };

    class TVTestSuite: public unittest::Suite {
    private:
    public:
      TVTestSuite(): unittest::Suite() {}

      TVTest tv_test(const std::string& name) {
        return TVTest(name).suite(*this);
      }
    };
  }
}

using namespace metajit;
using namespace metajit::test;

int main() {
  TVTestSuite suite;

  suite.tv_test("add").run({Type::Int32, Type::Int32}, [](Builder& builder) {
    return builder.build_add(builder.entry_arg(0), builder.entry_arg(1));
  }, [](z3::context& context, std::vector<tv::ValueState> args) {
    return args[0].value() + args[1].value();
  });

  suite.tv_test("branch").run({Type::Bool, Type::Int32, Type::Int32}, [](Builder& builder) {
    Block* true_block = builder.build_block();
    Block* false_block = builder.build_block();
    Block* cont_block = builder.build_block({Type::Int32});

    builder.build_branch(builder.entry_arg(0), true_block, false_block);

    builder.move_to_end(true_block);
    builder.build_jump(cont_block, {builder.entry_arg(1)});

    builder.move_to_end(false_block);
    builder.build_jump(cont_block, {builder.entry_arg(2)});

    builder.move_to_end(cont_block);
    return cont_block->arg(0);
  }, [](z3::context& context, std::vector<tv::ValueState> args) {
    return z3::ite(args[0].value().bit2bool(0), args[1].value(), args[2].value());
  });

  suite.tv_test("abs_branch").run({Type::Int32}, [](Builder& builder) {
    Block* true_block = builder.build_block();
    Block* false_block = builder.build_block();
    Block* cont_block = builder.build_block({Type::Int32});

    Value* is_neg = builder.build_lt_s(builder.entry_arg(0), builder.build_const(Type::Int32, 0));
    builder.build_branch(is_neg, true_block, false_block);

    builder.move_to_end(true_block);
    Value* negated = builder.build_sub(builder.build_const(Type::Int32, 0), builder.entry_arg(0));
    builder.build_jump(cont_block, {negated});

    builder.move_to_end(false_block);
    builder.build_jump(cont_block, {builder.entry_arg(0)});

    builder.move_to_end(cont_block);
    return cont_block->arg(0);
  }, [](z3::context& context, std::vector<tv::ValueState> args) {
    return z3::ite(z3::slt(args[0].value(), context.bv_val(0, 32)), -args[0].value(), args[0].value());
  });

  suite.tv_test("abs_select").run({Type::Int32}, [](Builder& builder) {
    Value* is_neg = builder.build_lt_s(builder.entry_arg(0), builder.build_const(Type::Int32, 0));
    Value* negated = builder.build_sub(builder.build_const(Type::Int32, 0), builder.entry_arg(0));
    return builder.build_select(is_neg, negated, builder.entry_arg(0));
  }, [](z3::context& context, std::vector<tv::ValueState> args) {
    return z3::ite(z3::slt(args[0].value(), context.bv_val(0, 32)), -args[0].value(), args[0].value());
  });

  /*
  suite.tv_test("store_load").run({Type::Ptr, Type::Int32}, [](Builder& builder) {
    Value* ptr = builder.entry_arg(0);
    Value* value = builder.entry_arg(1);
    builder.build_store(ptr, value, AliasingGroup(0), 0);
    return builder.build_load(ptr, Type::Int32, LoadFlags::None, AliasingGroup(0), 0);
  }, [](z3::context& context, std::vector<tv::ValueState> args) {
    return args[1].value();
  });
  */

  return suite.finish();
}