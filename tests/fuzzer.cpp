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

#include <iostream>
#include "../jitir.hpp"
#include "../tv.hpp"
#include "diff.hpp"

namespace metajit {
  namespace test {
    template<typename DataType = TestData>
    class Fuzzer {
    private:
      Builder* _builder = nullptr;
      DataType* _data = nullptr;
      std::vector<Value*> _all_values;
      std::vector<Value*> _tv_inputs; // pre-allocated inputs for TVTestData
      size_t _depth = 0;

      size_t _max_depth = 16;

      Type gen_type() {
        Type res = (Type) (rand() % 6 + 1);
        // don't generate Float types
        if (res == Type::Float32) {
          res = Type::Ptr;
        }
        return res;
      }

      Type gen_int_type() {
        return (Type) (rand() % 4 + (size_t) Type::Int8);
      }

      Type gen_int_or_bool_type() {
        return (Type) (rand() % 5 + (size_t) Type::Bool);
      }

      Value* gen_int(RandomRange random_range) {
        switch (rand() % 13) {
          #define binop(name) \
            return _builder->build_##name( \
              gen(RandomRange(random_range.type())), \
              gen(RandomRange(random_range.type())) \
            ); \

          #define shift(name) \
            return _builder->build_##name( \
              gen(RandomRange(random_range.type())), \
              _builder->build_and(\
                gen(RandomRange(random_range.type())), \
                _builder->build_const(random_range.type(), type_width(random_range.type()) - 1)) \
            ); \

          case 0: binop(add)
          case 1: binop(sub)
          case 2: binop(mul)
          case 3: binop(and)
          case 4: binop(or)
          case 5: binop(xor)
          case 6:
            return _builder->build_select(
              gen(RandomRange(Type::Bool)),
              gen(random_range),
              gen(random_range)
            );
          case 7: shift(shl)
          case 8: shift(shr_u)
          case 9: shift(shr_s)
          case 10:
            return _builder->build_resize_u(
              gen(RandomRange(gen_int_or_bool_type())),
              random_range.type()
            );
          case 11:
            return _builder->build_resize_s(
              gen(RandomRange(gen_int_or_bool_type())),
              random_range.type()
            );
          case 12:
            // AssumeConst is can lead to misoptimization in genext testing
            if constexpr (std::is_same_v<DataType, TraceTestData>) {
              return gen(random_range);
            } else {
              return _builder->build_assume_const(
                gen(random_range)
              );
            }
          default:
            assert(false && "Unreachable");

          #undef binop
          #undef shift
        }
      }

      Value* gen_bool(RandomRange random_range) {
        switch (rand() % 8) {
          #define cmp(name) { \
            Type type = gen_int_type(); \
            return _builder->build_##name( \
              gen(RandomRange(type)), \
              gen(RandomRange(type)) \
            ); \
          }

          #define binop(name) \
            return _builder->build_##name( \
              gen(RandomRange(Type::Bool)), \
              gen(RandomRange(Type::Bool)) \
            ); \

          case 0: cmp(eq)
          case 1: cmp(lt_u)
          case 2: cmp(lt_s)
          case 3: binop(and)
          case 4: binop(or)
          case 5: binop(xor)
          case 6:
            return _builder->build_select(
              gen(RandomRange(Type::Bool)),
              gen(random_range),
              gen(random_range)
            );
          case 7:
            if constexpr (std::is_same_v<DataType, TraceTestData>) {
              return gen(random_range);
            } else {
              return _builder->build_assume_const(
                gen(random_range)
              );
            }
          default:
            assert(false && "Unreachable");

          #undef cmp
          #undef binop
        }
      }

      Value* gen_input(RandomRange random_range) {
        if constexpr (std::is_same_v<DataType, TraceTestData>) {
          if (rand() % 3 == 0) {  // 33% chance of static input for genext testing
            return _data->static_input(random_range);
          }
        }
        if constexpr (std::is_same_v<DataType, tv::TVTestData>) {
          // Pick a random pre-allocated input of the right type
          for (size_t i = rand() % _tv_inputs.size(); i < _tv_inputs.size(); i++) {
            if (_tv_inputs[i]->type() == random_range.type()) {
              return _tv_inputs[i];
            }
          }
          // Wrap around from the beginning
          for (Value* input : _tv_inputs) {
            if (input->type() == random_range.type()) {
              return input;
            }
          }
          // Fallback: no input of this type, return a constant
          return random_range.gen_const(*_builder);
        } else {
          return _data->input(random_range);
        }
      }

      Value* gen_ptr(RandomRange random_range) {
        if constexpr (std::is_same_v<DataType, tv::TVTestData>) {
          // For TVTestData, the only valid pointer is the output buffer,
          // which we don't expose for computation. Just return a constant.
          return random_range.gen_const(*_builder);
        }
        switch (rand() % 4) {
          case 0: return gen_input(random_range);
          case 1:
            return _builder->build_add_ptr(
              gen(RandomRange(Type::Ptr)),
              gen(RandomRange(Type::Int64))
            );
          case 2:
            return _builder->build_select(
              gen(RandomRange(Type::Bool)),
              gen(random_range),
              gen(random_range)
            );
          case 3:
            if constexpr (std::is_same_v<DataType, TraceTestData>) {
              return gen_input(random_range);
            } else {
              return _builder->build_assume_const(
                gen(random_range)
              );
            }
          default:
            assert(false && "Unreachable");
        }
      }

      Value* build_is_inside(Value* value, const RandomRange& range) {
        Value* ge_min = _builder->fold_ge_u(
          value,
          _builder->build_const(range.type(), range.min())
        );
        Value* le_max = _builder->fold_le_u(
          value,
          _builder->build_const(range.type(), range.max())
        );
        return _builder->build_and(ge_min, le_max);
      }

      Value* gen(RandomRange random_range) {
        _depth++;
        if (_depth >= _max_depth) {
          _depth--;
          return gen_input(random_range);
        }

        Value* result = nullptr;
        if (random_range.type() == Type::Ptr) {
          result = gen_ptr(random_range);
        } else if (_depth > 1 && _all_values.size() && rand() % 10 == 0) {
          // in 10% of cases try to re-use a previous result
          for (size_t i = rand() % _all_values.size(); i < _all_values.size(); i++) {
            if (_all_values[i]->type() == random_range.type()) {
              result = _all_values[i];
              break;
            }
          }
        }
        if (!result) {
          switch (rand() % 9) {
            case 0: result = random_range.gen_const(*_builder); break;
            case 1: result = gen_input(random_range); break;
            default:
              switch (random_range.type()) {
                case Type::Bool:
                  result = gen_bool(random_range);
                break;
                case Type::Int8:
                case Type::Int16:
                case Type::Int32:
                case Type::Int64:
                  result = gen_int(random_range);
                break;
                default: assert(false && "Unreachable");
              }
          }
        }

        assert(result);
        assert(result->type() == random_range.type());

        if (!random_range.is_full()) {
          assert(is_int(result->type()));

          Block* else_block = _builder->build_block();
          Block* then_block = _builder->build_block();

          _builder->build_branch(
            build_is_inside(result, random_range),
            then_block,
            else_block
          );

          _builder->move_to_end(else_block);
          _builder->build_exit();

          _builder->move_to_end(then_block);
        }

        _depth--;
        if (dynmatch(Inst, inst, result)) {
          _all_values.push_back(result);
        }
        return result;
      }
    public:
      Fuzzer() {}

      void run() {
        
      }

      bool run_once() {
        if constexpr (std::is_same_v<DataType, tv::TVTestData>) {
          return run_once_tv();
        } else {
          Context context;
          Allocator allocator;
          Section* section = new Section(context, allocator);

          _builder = new Builder(section);
          _builder->move_to_end(_builder->build_block({
            Type::Ptr
          }));

          _data = new DataType(*_builder);
          _depth = 0;
          _data->output(gen(RandomRange(gen_type())));
          if (_all_values.size()) {
            for (int i = 0; i < 5; i++) {
              // output five random other values
              _data->output(_all_values[rand() % _all_values.size()]);
            }
          }
          assert(_depth == 0);

          _builder->build_exit();

          if (section->verify(std::cout)) {
            exit(1);
          }

          bool result = true;

          try {
            if constexpr (std::is_same_v<DataType, TraceTestData>) {
              check_trace_differential("", section, *_data, 4, 256);
            } else {
              check_codegen_differential(
                "",
                section,
                *_data,
                2048,
                true,
                /*verify_interpreter=*/ true,
                /*verify_aot=*/ false
              );
            }
          } catch (unittest::AssertionError& err) {
            section->write(std::cout);
            std::cerr << "Test failed: " << err.message() << std::endl;
            result = false;
          }

          delete _data;
          _data = nullptr;
          delete _builder;
          _builder = nullptr;
          _all_values.clear();
          delete section;
          return result;
        }
      }

      bool run_once_tv() {
        // Pre-decide input types: 4 inputs of varied non-pointer types
        std::vector<Type> input_types = {
          gen_int_or_bool_type(), gen_int_or_bool_type(),
          gen_int_or_bool_type(), gen_int_or_bool_type()
        };

        Context context;
        Allocator allocator;
        Section* section = new Section(context, allocator);

        _builder = new Builder(section);
        _data = new tv::TVTestData(*_builder, input_types);

        // Populate _tv_inputs from the entry args
        _tv_inputs.clear();
        for (size_t i = 0; i < input_types.size(); i++) {
          _tv_inputs.push_back(_data->input(i));
        }

        _depth = 0;
        // Generate outputs using non-pointer types
        Type out_type = gen_int_or_bool_type();
        _data->output(gen(RandomRange(out_type)));
        // Collect non-pointer values for additional outputs
        std::vector<Value*> non_ptr_values;
        for (Value* v : _all_values) {
          if (v->type() != Type::Ptr) {
            non_ptr_values.push_back(v);
          }
        }
        if (non_ptr_values.size()) {
          for (int i = 0; i < 5; i++) {
            _data->output(non_ptr_values[rand() % non_ptr_values.size()]);
          }
        }
        assert(_depth == 0);

        _builder->build_exit();

        if (section->verify(std::cout)) {
          exit(1);
        }

        // Clone and optimize
        Section* optimized = new Section(context, allocator);
        Clone::run(section, optimized);
        Simplify::run(optimized, 10);
        DeadCodeElim::run(optimized);

        bool result = true;
        try {
          tv::check_tv_refinement(section, optimized, *_data);
        } catch (std::runtime_error& err) {
          section->write(std::cout);
          std::cerr << "Optimized:\n";
          optimized->write(std::cout);
          std::cerr << "TV check failed: " << err.what() << std::endl;
          result = false;
        }

        delete _data;
        _data = nullptr;
        delete _builder;
        _builder = nullptr;
        _all_values.clear();
        _tv_inputs.clear();
        delete section;
        delete optimized;
        return result;
      }
    };
  }
}

namespace cl = llvm::cl;

cl::OptionCategory category("Options");

cl::opt<int> seed("seed", cl::desc("seed for the rng, to reproduce crashes"), cl::cat(category));
cl::opt<int> number_of_runs("number-of-runs", cl::desc("How many random programs to test (default is run forever)"), cl::cat(category));

enum class FuzzerMode { Regular, Trace, TV };
cl::opt<FuzzerMode> mode("mode",
  cl::desc("Fuzzer mode"),
  cl::values(
    clEnumValN(FuzzerMode::Regular, "regular", "Test codegen correctness (default)"),
    clEnumValN(FuzzerMode::Trace,   "trace",   "Test generating extensions (meta-tracing)"),
    clEnumValN(FuzzerMode::TV,      "tv",      "Test optimization correctness using translation validation")
  ),
  cl::init(FuzzerMode::Regular),
  cl::cat(category));

template<typename FuzzerType>
int run_fuzzer_loop(FuzzerType& fuzzer, int seed_val, int runs) {
  using namespace metajit;

  if (!seed_val) {
    srand(time(NULL));
  } else {
    srand(seed_val);
    fuzzer.run_once();
    return 0;
  }

  bool limited = runs > 0;
  if (!runs) {
    runs = -1;
  }

  int passed = 0;
  std::vector<int> failed_seeds;

  for (int i = 0; i != runs; i++) {
    int curr_seed = rand();
    srand(curr_seed);
    std::cout << "seed: " << curr_seed << "\n";
    if (fuzzer.run_once()) {
      passed++;
    } else {
      failed_seeds.push_back(curr_seed);
      if (!limited) {
        return -1;
      }
    }
  }

  if (limited) {
    int failed = (int) failed_seeds.size();
    std::cout << passed << " passed, " << failed << " failed";
    if (failed) {
      std::cout << ", seeds:";
      for (int s : failed_seeds) {
        std::cout << " " << s;
      }
    }
    std::cout << "\n";
    return failed ? -1 : 0;
  }

  return 0;
}

int main(int argc, char** argv) {
  using namespace metajit;
  using namespace metajit::test;
  cl::HideUnrelatedOptions(category);
  cl::ParseCommandLineOptions(argc, argv, "fuzzer");

  metajit::LLVMCodeGen::initilize_llvm_jit();

  switch (mode) {
    case FuzzerMode::Trace: {
      Fuzzer<TraceTestData> fuzzer;
      return run_fuzzer_loop(fuzzer, seed.getValue(), number_of_runs.getValue());
    }
    case FuzzerMode::TV: {
      Fuzzer<tv::TVTestData> fuzzer;
      return run_fuzzer_loop(fuzzer, seed.getValue(), number_of_runs.getValue());
    }
    case FuzzerMode::Regular: {
      Fuzzer<TestData> fuzzer;
      return run_fuzzer_loop(fuzzer, seed.getValue(), number_of_runs.getValue());
    }
  }
}
