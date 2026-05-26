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

    private:
      // Shared setup: builds the section, creates z3 entry args and codegen,
      // then calls check(codegen, section) for the actual assertions.
      void run_impl(std::vector<Type> entry_args,
                    std::function<void(Builder&)> build,
                    std::function<void(tv::Z3CodeGen&, Section*)> check) {
        unittest::BaseTest<TVTest>::run([&]() {
          Context context;
          Allocator allocator;
          Section* section = new Section(context, allocator);

          Builder builder(section);
          builder.move_to_end(builder.build_block(entry_args));
          build(builder);

          section->autoname();

          z3::context z3_context;

          std::vector<std::optional<size_t>> regions;
          for (Type type : entry_args) {
            regions.emplace_back();
          }
          tv::MemoryState memory_state(z3_context, regions);

          std::vector<tv::ValueState> z3_entry_args;
          size_t region_id = 0;

          for (size_t it = 0; it < entry_args.size(); it++) {
            tv::ValueState arg_state(
              entry_args[it],
              z3_context.bv_const(
                ("arg" + std::to_string(it)).c_str(),
                type_width(entry_args[it])
              )
            );
            if (entry_args[it] == Type::Ptr) {
              arg_state.set_provenance(z3_context.bv_val(
                region_id++, memory_state.provenance_width()
              ));
            }
            z3_entry_args.push_back(arg_state);
          }

          tv::Z3CodeGen codegen(section, z3_context, z3_entry_args, memory_state);
          check(codegen, section);
        });
      }

      void check_expr(tv::Z3CodeGen& codegen, Section* section,
                      const char* field, z3::expr tv_expr, z3::expr expected_expr) {
        z3::context& z3_context = tv_expr.ctx();
        z3::solver solver(z3_context);
        solver.add((tv_expr != expected_expr).simplify());
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
          stream << "Field: " << field << "\n";
          stream << "TV: " << model.eval(tv_expr, true) << "\n";
          stream << "Expected: " << model.eval(expected_expr, true) << "\n";
          throw unittest::AssertionError(
            "Counterexample found",
            __LINE__,
            __FILE__,
            stream.str()
          );
        } else if (check_result != z3::unsat) {
          throw unittest::AssertionError("Failed to prove", __LINE__, __FILE__);
        }
      }

      std::vector<tv::ValueState> entry_arg_states(tv::Z3CodeGen& codegen, Section* section) {
        std::vector<tv::ValueState> z3_entry_args;
        for (Arg* arg : section->entry()->args()) {
          z3_entry_args.push_back(codegen.emit(arg));
        }
        return z3_entry_args;
      }

    public:
      void run(std::vector<Type> entry_args,
               std::function<Value*(Builder&)> build,
               std::function<z3::expr(z3::context&, std::vector<tv::ValueState>)> expected) {
        Value* result = nullptr;
        run_impl(entry_args, [&](Builder& builder) {
          result = build(builder);
          builder.build_exit();
        }, [&](tv::Z3CodeGen& codegen, Section* section) {
          z3::context& z3_context = codegen.emit(result).value().ctx();
          check_expr(codegen, section, "value",
            codegen.emit(result).value(),
            expected(z3_context, entry_arg_states(codegen, section)));
        });
      }

      // Like run(), but checks the full ValueState (both value and is_poison).
      // The expected lambda returns a tv::ValueState with both fields set.
      void run_valuestate(std::vector<Type> entry_args,
               std::function<Value*(Builder&)> build,
               std::function<tv::ValueState(z3::context&, std::vector<tv::ValueState>)> expected) {
        Value* result = nullptr;
        run_impl(entry_args, [&](Builder& builder) {
          result = build(builder);
          builder.build_exit();
        }, [&](tv::Z3CodeGen& codegen, Section* section) {
          tv::ValueState tv_result = codegen.emit(result);
          z3::context& z3_context = tv_result.value().ctx();
          tv::ValueState expected_result = expected(z3_context, entry_arg_states(codegen, section));
          check_expr(codegen, section, "value", tv_result.value(), expected_result.value());
          check_expr(codegen, section, "is_poison", tv_result.is_poison(), expected_result.is_poison());
        });
      }

      // Checks the UB flag of the whole section, without checking any result value.
      void run_ub(std::vector<Type> entry_args,
               std::function<void(Builder&)> build,
               std::function<z3::expr(z3::context&, std::vector<tv::ValueState>)> expected_ub) {
        run_impl(entry_args, build,
          [&](tv::Z3CodeGen& codegen, Section* section) {
            z3::context& z3_context = codegen.has_ub().ctx();
            check_expr(codegen, section, "has_ub",
              codegen.has_ub(),
              expected_ub(z3_context, entry_arg_states(codegen, section)));
          }
        );
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

  suite.tv_test("store_load").run({Type::Ptr, Type::Int32}, [](Builder& builder) {
    Value* ptr = builder.entry_arg(0);
    Value* value = builder.entry_arg(1);
    builder.build_store(ptr, value, AliasingGroup(0), 0);
    return builder.build_load(ptr, Type::Int32, LoadFlags::None, AliasingGroup(0), 0);
  }, [](z3::context& context, std::vector<tv::ValueState> args) {
    return args[1].value();
  });

  suite.tv_test("abs_branch_memory").run({Type::Int32, Type::Ptr}, [](Builder& builder) {
    Block* true_block = builder.build_block();
    Block* false_block = builder.build_block();
    Block* cont_block = builder.build_block();

    Value* is_neg = builder.build_lt_s(builder.entry_arg(0), builder.build_const(Type::Int32, 0));
    builder.build_branch(is_neg, true_block, false_block);

    builder.move_to_end(true_block);
    Value* negated = builder.build_sub(builder.build_const(Type::Int32, 0), builder.entry_arg(0));
    builder.build_store(builder.entry_arg(1), negated, AliasingGroup(0), 0);
    builder.build_jump(cont_block);

    builder.move_to_end(false_block);
    builder.build_store(builder.entry_arg(1), builder.entry_arg(0), AliasingGroup(0), 0);
    builder.build_jump(cont_block);

    builder.move_to_end(cont_block);
    return builder.build_load(builder.entry_arg(1), Type::Int32, LoadFlags::None, AliasingGroup(0), 0);
  }, [](z3::context& context, std::vector<tv::ValueState> args) {
    return z3::ite(z3::slt(args[0].value(), context.bv_val(0, 32)), -args[0].value(), args[0].value());
  });

  // Non-poison values have is_poison == false
  suite.tv_test("add_not_poison").run_valuestate({Type::Int32, Type::Int32}, [](Builder& builder) {
    return builder.build_add(builder.entry_arg(0), builder.entry_arg(1));
  }, [](z3::context& context, std::vector<tv::ValueState> args) {
    tv::ValueState result(Type::Int32, args[0].value() + args[1].value());
    // is_poison defaults to false
    return result;
  });

  // Poison literal has is_poison == true and can be any concrete value
  suite.tv_test("poison_literal").run_valuestate({}, [](Builder& builder) {
    return builder.section()->context().build_poison(Type::Int32);
  }, [](z3::context& context, std::vector<tv::ValueState> args) {
    tv::ValueState result(Type::Int32, context.bv_val(0, 32));
    result.set_poison(context.bool_val(true));
    return result;
  });

  // Division/modulo by zero produces poison
  suite.tv_test("divu_nonzero_not_poison").run_valuestate({Type::Int32, Type::Int32}, [](Builder& builder) {
    return builder.build_div_u(builder.entry_arg(0), builder.entry_arg(1));
  }, [](z3::context& context, std::vector<tv::ValueState> args) {
    tv::ValueState result(Type::Int32, z3::udiv(args[0].value(), args[1].value()));
    result.set_poison(args[1].value() == context.bv_val(0, 32));
    return result;
  });

  suite.tv_test("divs_nonzero_not_poison").run_valuestate({Type::Int32, Type::Int32}, [](Builder& builder) {
    return builder.build_div_s(builder.entry_arg(0), builder.entry_arg(1));
  }, [](z3::context& context, std::vector<tv::ValueState> args) {
    tv::ValueState result(Type::Int32, args[0].value() / args[1].value());
    result.set_poison(args[1].value() == context.bv_val(0, 32));
    return result;
  });

  suite.tv_test("modu_nonzero_not_poison").run_valuestate({Type::Int32, Type::Int32}, [](Builder& builder) {
    return builder.build_mod_u(builder.entry_arg(0), builder.entry_arg(1));
  }, [](z3::context& context, std::vector<tv::ValueState> args) {
    tv::ValueState result(Type::Int32, z3::urem(args[0].value(), args[1].value()));
    result.set_poison(args[1].value() == context.bv_val(0, 32));
    return result;
  });

  suite.tv_test("mods_nonzero_not_poison").run_valuestate({Type::Int32, Type::Int32}, [](Builder& builder) {
    return builder.build_mod_s(builder.entry_arg(0), builder.entry_arg(1));
  }, [](z3::context& context, std::vector<tv::ValueState> args) {
    tv::ValueState result(Type::Int32, z3::srem(args[0].value(), args[1].value()));
    result.set_poison(args[1].value() == context.bv_val(0, 32));
    return result;
  });

  // Shifts with amount >= type width produce poison
  suite.tv_test("shl_overflow_poison").run_valuestate({Type::Int32, Type::Int32}, [](Builder& builder) {
    return builder.build_shl(builder.entry_arg(0), builder.entry_arg(1));
  }, [](z3::context& context, std::vector<tv::ValueState> args) {
    tv::ValueState result(Type::Int32, z3::shl(args[0].value(), args[1].value()));
    result.set_poison(z3::uge(args[1].value(), context.bv_val(32, 32)));
    return result;
  });

  suite.tv_test("shru_overflow_poison").run_valuestate({Type::Int32, Type::Int32}, [](Builder& builder) {
    return builder.build_shr_u(builder.entry_arg(0), builder.entry_arg(1));
  }, [](z3::context& context, std::vector<tv::ValueState> args) {
    tv::ValueState result(Type::Int32, z3::lshr(args[0].value(), args[1].value()));
    result.set_poison(z3::uge(args[1].value(), context.bv_val(32, 32)));
    return result;
  });

  suite.tv_test("shrs_overflow_poison").run_valuestate({Type::Int32, Type::Int32}, [](Builder& builder) {
    return builder.build_shr_s(builder.entry_arg(0), builder.entry_arg(1));
  }, [](z3::context& context, std::vector<tv::ValueState> args) {
    tv::ValueState result(Type::Int32, z3::ashr(args[0].value(), args[1].value()));
    result.set_poison(z3::uge(args[1].value(), context.bv_val(32, 32)));
    return result;
  });

  // Poison propagates through ordinary instructions
  suite.tv_test("add_poison_propagates").run_valuestate({Type::Int32}, [](Builder& builder) {
    Value* p = builder.section()->context().build_poison(Type::Int32);
    return builder.build_add(builder.entry_arg(0), p);
  }, [](z3::context& context, std::vector<tv::ValueState> args) {
    tv::ValueState result(Type::Int32, args[0].value() + context.bv_val(0, 32));
    result.set_poison(context.bool_val(true));
    return result;
  });

  // Select: poison condition -> poison result (concrete value is arbitrary, use ite)
  suite.tv_test("select_poison_cond").run_valuestate({Type::Int32, Type::Int32}, [](Builder& builder) {
    Value* p = builder.section()->context().build_poison(Type::Bool);
    return builder.build_select(p, builder.entry_arg(0), builder.entry_arg(1));
  }, [](z3::context& context, std::vector<tv::ValueState> args) {
    // cond is poison so value is arbitrary; we use bv_val(0,1) as the poison cond's concrete value
    tv::ValueState result(Type::Int32,
      z3::ite(context.bv_val(0, 1).bit2bool(0), args[0].value(), args[1].value()));
    result.set_poison(context.bool_val(true));
    return result;
  });

  // Select: poison on selected branch -> poison result
  suite.tv_test("select_poison_selected_branch").run_valuestate({Type::Bool, Type::Int32}, [](Builder& builder) {
    Value* p = builder.section()->context().build_poison(Type::Int32);
    // true branch is poison, false branch is arg 1
    return builder.build_select(builder.entry_arg(0), p, builder.entry_arg(1));
  }, [](z3::context& context, std::vector<tv::ValueState> args) {
    // concrete value: ite(cond, 0 (poison's arbitrary value), arg1)
    tv::ValueState result(Type::Int32,
      z3::ite(args[0].value().bit2bool(0), context.bv_val(0, 32), args[1].value()));
    // poison iff cond is true (the poison branch was selected)
    result.set_poison(args[0].value().bit2bool(0));
    return result;
  });

  // Select: poison on non-selected branch -> result is NOT poison
  suite.tv_test("select_poison_nonselected_branch").run_valuestate({Type::Bool, Type::Int32}, [](Builder& builder) {
    Value* p = builder.section()->context().build_poison(Type::Int32);
    // true branch is arg 1, false branch is poison
    return builder.build_select(builder.entry_arg(0), builder.entry_arg(1), p);
  }, [](z3::context& context, std::vector<tv::ValueState> args) {
    // concrete value: ite(cond, arg1, 0 (poison's arbitrary value))
    tv::ValueState result(Type::Int32,
      z3::ite(args[0].value().bit2bool(0), args[1].value(), context.bv_val(0, 32)));
    // poison iff cond is false (the poison branch was selected)
    result.set_poison(!args[0].value().bit2bool(0));
    return result;
  });

  // Branch on non-poison: no UB
  suite.tv_test("branch_no_ub").run_ub({Type::Bool}, [](Builder& builder) {
    Block* true_block = builder.build_block();
    Block* false_block = builder.build_block();
    builder.build_branch(builder.entry_arg(0), true_block, false_block);
    builder.move_to_end(true_block);
    builder.build_exit();
    builder.move_to_end(false_block);
    builder.build_exit();
  }, [](z3::context& context, std::vector<tv::ValueState> args) {
    return context.bool_val(false);
  });

  // Branch on poison: always UB
  suite.tv_test("branch_poison_ub").run_ub({}, [](Builder& builder) {
    Block* true_block = builder.build_block();
    Block* false_block = builder.build_block();
    Value* p = builder.section()->context().build_poison(Type::Bool);
    builder.build_branch(p, true_block, false_block);
    builder.move_to_end(true_block);
    builder.build_exit();
    builder.move_to_end(false_block);
    builder.build_exit();
  }, [](z3::context& context, std::vector<tv::ValueState> args) {
    return context.bool_val(true);
  });

  // Store of poison value: UB
  suite.tv_test("store_poison_value_ub").run_ub({Type::Ptr}, [](Builder& builder) {
    Value* p = builder.section()->context().build_poison(Type::Int32);
    builder.build_store(builder.entry_arg(0), p, AliasingGroup(0), 0);
    builder.build_exit();
  }, [](z3::context& context, std::vector<tv::ValueState> args) {
    return context.bool_val(true);
  });

  // Store to poison pointer: UB
  suite.tv_test("store_poison_ptr_ub").run_ub({Type::Int32}, [](Builder& builder) {
    Value* p = builder.section()->context().build_poison(Type::Ptr);
    builder.build_store(p, builder.entry_arg(0), AliasingGroup(0), 0);
    builder.build_exit();
  }, [](z3::context& context, std::vector<tv::ValueState> args) {
    return context.bool_val(true);
  });

  // Load from poison pointer: UB
  suite.tv_test("load_poison_ptr_ub").run_ub({}, [](Builder& builder) {
    Value* p = builder.section()->context().build_poison(Type::Ptr);
    builder.build_load(p, Type::Int32, LoadFlags::None, AliasingGroup(0), 0);
    builder.build_exit();
  }, [](z3::context& context, std::vector<tv::ValueState> args) {
    return context.bool_val(true);
  });

  // Conditional UB: branch on result of div, which is poison iff divisor is zero
  suite.tv_test("branch_conditional_poison_ub").run_ub({Type::Int32, Type::Int32}, [](Builder& builder) {
    Block* true_block = builder.build_block();
    Block* false_block = builder.build_block();
    Value* div = builder.build_div_u(builder.entry_arg(0), builder.entry_arg(1));
    Value* is_big = builder.build_lt_u(builder.build_const(Type::Int32, 100), div);
    builder.build_branch(is_big, true_block, false_block);
    builder.move_to_end(true_block);
    builder.build_exit();
    builder.move_to_end(false_block);
    builder.build_exit();
  }, [](z3::context& context, std::vector<tv::ValueState> args) {
    // UB iff divisor is zero (div result is poison, which is then branched on)
    return args[1].value() == context.bv_val(0, 32);
  });

  return suite.finish();
}