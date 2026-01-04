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

#include "../jitir.hpp"
#include "diff.hpp"

namespace metajit {
  namespace test {
    class Fuzzer {
    private:
      Builder* _builder = nullptr;
      TestData* _data = nullptr;
      size_t _depth = 0;

      size_t _max_depth = 16;

      Type gen_type() {
        return (Type) (rand() % 6 + 1);
      }

      Type gen_int_type() {
        return (Type) (rand() % 4 + (size_t) Type::Int8);
      }

      Type gen_int_or_bool_type() {
        return (Type) (rand() % 5 + (size_t) Type::Bool);
      }

      Value* gen_int(RandomRange random_range) {
        switch (rand() % 12) {
          #define binop(name) \
            return _builder->build_##name( \
              gen(RandomRange(random_range.type())), \
              gen(RandomRange(random_range.type())) \
            ); \
          
          #define shift(name) \
            return _builder->build_##name( \
              gen(RandomRange(random_range.type())), \
              gen(RandomRange(random_range.type(), 0, type_size(random_range.type()) * 8 - 1)) \
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
          default:
            assert(false && "Unreachable");
          
          #undef binop
          #undef shift
        }
      }

      Value* gen_bool(RandomRange random_range) {
        switch (rand() % 7) {
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
          default:
            assert(false && "Unreachable");

          #undef cmp
          #undef binop
        }
      }

      Value* gen_ptr(RandomRange random_range) {
        switch (rand() % 3) {
          case 0: return _data->input(random_range);
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
        return _builder->fold_and(ge_min, le_max);
      }

      Value* gen(RandomRange random_range) {
        _depth++;
        if (_depth >= _max_depth) {
          _depth--;
          return _data->input(random_range);
        }

        Value* result = nullptr;
        if (random_range.type() == Type::Ptr) {
          result = gen_ptr(random_range);
        } else {
          switch (rand() % 8) {
            case 0: result = random_range.gen_const(*_builder); break;
            case 1: result = _data->input(random_range); break;
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

          _builder->fold_branch(
            build_is_inside(result, random_range),
            then_block,
            else_block
          );

          _builder->move_to_end(else_block);
          _builder->build_exit();

          _builder->move_to_end(then_block);
        }

        _depth--;
        return result;
      }
    public:
      Fuzzer() {}

      void run() {
        
      }

      void run_once() {
        Context context;
        Allocator allocator;
        Section* section = new Section(context, allocator);

        _builder = new Builder(section);
        _builder->move_to_end(_builder->build_block());

        _data = new TestData(*_builder);

        _depth = 0;
        _data->output(gen(RandomRange(gen_type())));
        assert(_depth == 0);

        _builder->build_exit();

        if (section->verify(std::cout)) {
          exit(1);
        }

        try {
          check_codegen_differential("", section, *_data);
        } catch (unittest::AssertionError& err) {
          section->write(std::cout);
          std::cerr << "Test failed: " << err.message() << std::endl;
        }

        delete _data;
        _data = nullptr;
        delete _builder;
        _builder = nullptr;

        delete section;
      }
    };
  }
}

int main() {
  using namespace metajit;
  using namespace metajit::test;

  metajit::LLVMCodeGen::initilize_llvm_jit();

  srand(1234);

  Fuzzer fuzzer;
  while (true) {
    fuzzer.run_once();
  }

  return 0;
}