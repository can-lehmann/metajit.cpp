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
      TVTestSuite(int argc = 0, char** argv = nullptr): unittest::Suite(argc, argv) {}

      TVTest tv_test(const std::string& name) {
        return TVTest(name).suite(*this);
      }
    };
  }
}

using namespace metajit;
using namespace metajit::test;

void test_signed_divmod_poison(TVTestSuite& suite) {
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    unsigned width = type_width(type);
    uint64_t minimum = uint64_t(1) << (width - 1);
    uint64_t minus_one = type_mask(type);
    for (bool remainder : {false, true}) {
      std::string prefix = std::string("signed_divmod_") + (remainder ? "mod_" : "div_") + std::to_string(width);
      auto build = [=](Builder& builder, Value* a, Value* b) -> Value* {
        return remainder ? (Value*) builder.build_mod_s(a, b) : (Value*) builder.build_div_s(a, b);
      };
      suite.tv_test(prefix + "_symbolic").run_valuestate({type, type},
        [=](Builder& builder) {
          return build(builder, builder.entry_arg(0), builder.entry_arg(1));
        }, [=](z3::context& context, std::vector<tv::ValueState> args) {
          z3::expr a = args[0].value();
          z3::expr b = args[1].value();
          tv::ValueState result(type, remainder ? z3::srem(a, b) : a / b);
          result.set_poison(b == context.bv_val(0, width) ||
            (a == context.bv_val(minimum, width) && b == context.bv_val(minus_one, width)));
          return result;
        });
      suite.tv_test(prefix + "_zero_divisor").run_valuestate({type},
        [=](Builder& builder) {
          return build(builder, builder.entry_arg(0), builder.build_const(type, 0));
        }, [=](z3::context& context, std::vector<tv::ValueState> args) {
          z3::expr a = args[0].value();
          z3::expr zero = context.bv_val(0, width);
          tv::ValueState result(type, remainder ? z3::srem(a, zero) : a / zero);
          result.set_poison(context.bool_val(true));
          return result;
        });
      struct Boundary {
        const char* name;
        uint64_t a;
        uint64_t b;
        bool poison;
        uint64_t quotient;
        uint64_t rest;
      };
      for (Boundary boundary : {
          Boundary{"overflow", minimum, minus_one, true, minimum, 0},
          Boundary{"minimum_by_one", minimum, 1, false, minimum, 0},
          Boundary{"adjacent", minimum + 1, minus_one, false, minimum - 1, 0},
          Boundary{"seven_by_two", 7, 2, false, 3, 1}}) {
        suite.tv_test(prefix + "_" + boundary.name).run_valuestate({},
          [=](Builder& builder) {
            return build(builder, builder.build_const(type, boundary.a), builder.build_const(type, boundary.b));
          }, [=](z3::context& context, std::vector<tv::ValueState>) {
            tv::ValueState result(type, context.bv_val(remainder ? boundary.rest : boundary.quotient, width));
            result.set_poison(context.bool_val(boundary.poison));
            return result;
          });
      }
      suite.tv_test(prefix + "_unsigned_patterns").run_valuestate({},
        [=](Builder& builder) -> Value* {
          Value* a = builder.build_const(type, minimum);
          Value* b = builder.build_const(type, minus_one);
          return remainder ? (Value*) builder.build_mod_u(a, b) : (Value*) builder.build_div_u(a, b);
        }, [=](z3::context& context, std::vector<tv::ValueState>) {
          return tv::ValueState(type, context.bv_val(remainder ? minimum : uint64_t(0), width));
        });
      for (bool freeze : {false, true}) {
        suite.tv_test(prefix + (freeze ? "_freeze_store" : "_store")).run_ub({Type::Ptr},
          [=](Builder& builder) {
            Value* result = build(builder, builder.build_const(type, minimum), builder.build_const(type, minus_one));
            if (freeze) result = builder.build_freeze(result);
            builder.build_store(builder.entry_arg(0), result, AliasingGroup(0), 0);
            builder.build_exit();
          }, [=](z3::context& context, std::vector<tv::ValueState>) {
            return context.bool_val(!freeze);
          });
      }
    }
  }
}

void test_freeze_output_relationship(TVTestSuite& suite) {
  for (bool division : {false, true}) {
    std::string name = std::string("simplify_freeze_output_relationship_") +
      (division ? "division" : "poison");
    suite.test(name).run([=]() {
      Context context;
      Allocator allocator;
      Section before(context, allocator);
      Builder builder(&before);
      Block* entry = builder.build_block({Type::Ptr, Type::Int8, Type::Int8});
      builder.move_to_end(entry);
      Value* value;
      if (division) {
        value = builder.build_div_u(entry->arg(1), entry->arg(2));
      } else {
        value = builder.build_poison(Type::Int8);
      }
      Value* masked = builder.build_and(value, builder.build_const(Type::Int8, 1));
      Value* frozen = builder.build_freeze(masked);
      Value* result = builder.build_and(frozen, builder.build_const(Type::Int8, 2));
      builder.build_store(entry->arg(0), frozen, AliasingGroup(0), 0);
      builder.build_store(entry->arg(0), result, AliasingGroup(0), 1);
      builder.build_exit();
      before.order_blocks(BlockOrdering::Dominator);
      unittest_assert(!before.verify(std::cout));

      Section after(context, allocator);
      Clone::run(&before, &after);
      after.order_blocks(BlockOrdering::Dominator);
      Simplify::run(&after, 4);
      unittest_assert(!after.verify(std::cout));

      z3::context z3_context;
      tv::MemoryState memory(z3_context, {std::nullopt});
      tv::ValueState data_ptr(Type::Ptr, z3_context.bv_const("data_ptr", type_width(Type::Ptr)));
      data_ptr.set_provenance(z3_context.bv_val(0, memory.provenance_width()));
      std::vector<tv::ValueState> args = {
        data_ptr,
        tv::ValueState(Type::Int8, z3_context.bv_const("numerator", 8)),
        tv::ValueState(Type::Int8, z3_context.bv_const("divisor", 8))
      };
      tv::ValueState second_ptr(Type::Ptr,
        data_ptr.value() + z3_context.bv_val(1, type_width(Type::Ptr)), data_ptr.provenance());
      for (Section* section : {&before, &after}) {
        tv::Z3CodeGen codegen(section, z3_context, args, memory);
        auto raw = codegen.exit_memory_state().load(data_ptr, Type::Int8);
        auto bits = codegen.exit_memory_state().load(second_ptr, Type::Int8);
        z3::solver solver(z3_context);
        solver.set("timeout", unsigned(5000));
        solver.add(codegen.has_ub() || raw.is_poison() || bits.is_poison() ||
                   bits.value() != (raw.value() & z3_context.bv_val(2, 8)));
        z3::check_result check_result = solver.check();
        if (check_result == z3::sat) {
          throw unittest::AssertionError("Freeze output relationship violated",
            __LINE__, __FILE__, solver.get_model().to_string());
        }
        unittest_assert(check_result == z3::unsat);
      }
    });
  }
}

int main(int argc, char** argv) {
  TVTestSuite suite(argc, argv);

  suite.test("simplify_preserves_select_poison_condition").run([]() {
    Context context;
    Allocator allocator;
    std::istringstream stream(R"(section {
b0(%0: Ptr, %1: Int8):
  %2 = And %1, 1:Int8
  %3 = Eq %2, 0:Int8
  %4 = Shl 0:Int8, 8:Int8
  %5 = Select %3, 0:Int8, %4
  %6 = And %5, 2:Int8
  %7 = Mul %6, 3:Int8
  %8 = And %7, 1:Int8
  Store %0, %8, aliasing=0, offset=0
  Exit
}
)");
    std::unique_ptr<Section> before(SectionReader<>::read_section(context, allocator, stream));
    before->order_blocks(BlockOrdering::Dominator);
    unittest_assert(!before->verify(std::cout));
    Section after(context, allocator);
    Clone::run(before.get(), &after);
    after.order_blocks(BlockOrdering::Dominator);
    Simplify::run(&after, 4);
    unittest_assert(!after.verify(std::cout));

    z3::context z3_context;
    tv::MemoryState memory(z3_context, {std::nullopt});
    tv::ValueState ptr(Type::Ptr, z3_context.bv_const("ptr", type_width(Type::Ptr)));
    ptr.set_provenance(z3_context.bv_val(0, memory.provenance_width()));
    std::vector<tv::ValueState> args = {
      ptr, tv::ValueState(Type::Int8, z3_context.bv_const("input", 8))
    };
    tv::Z3CodeGen original(before.get(), z3_context, args, memory);
    tv::Z3CodeGen optimized(&after, z3_context, args, memory);
    z3::solver solver(z3_context);
    solver.add(!original.has_ub() && optimized.has_ub());
    z3::check_result result = solver.check();
    if (result == z3::sat) {
      std::ostringstream message;
      message << solver.get_model() << "\nOptimized:\n";
      after.write(message);
      throw unittest::AssertionError("Simplify introduced UB through Select",
        __LINE__, __FILE__, message.str());
    }
    unittest_assert(result == z3::unsat);
  });

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
    result.set_poison(args[1].value() == context.bv_val(0, 32) ||
      (args[0].value() == context.bv_val(uint64_t(1) << 31, 32) &&
       args[1].value() == context.bv_val(uint64_t(0xffffffff), 32)));
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
    result.set_poison(args[1].value() == context.bv_val(0, 32) ||
      (args[0].value() == context.bv_val(uint64_t(1) << 31, 32) &&
       args[1].value() == context.bv_val(uint64_t(0xffffffff), 32)));
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

  // freeze strips poison: shl with out-of-range shift is poison, freeze makes it non-poison
  suite.tv_test("freeze_strips_poison").run_valuestate({Type::Int32, Type::Int32}, [](Builder& builder) {
    Value* shifted = builder.build_shl(builder.entry_arg(0), builder.entry_arg(1));
    return builder.build_freeze(shifted);
  }, [](z3::context& context, std::vector<tv::ValueState> args) {
    z3::expr is_poison = z3::uge(args[1].value(), context.bv_val(32, 32));
    z3::expr arbitrary = context.bv_const("freeze_0", 32);
    tv::ValueState result(Type::Int32,
      z3::ite(is_poison, arbitrary, z3::shl(args[0].value(), args[1].value())));
    // is_poison is false (freeze always produces a defined value)
    return result;
  });

  // freeze of a non-poison value is identity
  suite.tv_test("freeze_nonpoison_identity").run_valuestate({Type::Int32, Type::Int32}, [](Builder& builder) {
    Value* sum = builder.build_add(builder.entry_arg(0), builder.entry_arg(1));
    return builder.build_freeze(sum);
  }, [](z3::context& context, std::vector<tv::ValueState> args) {
    tv::ValueState result(Type::Int32, args[0].value() + args[1].value());
    // is_poison is false; value is unchanged since add is never poison
    return result;
  });

  test_signed_divmod_poison(suite);
  test_freeze_output_relationship(suite);

  return suite.finish();
}
