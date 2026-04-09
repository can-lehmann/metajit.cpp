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

void check_roundtrip(Section* section) {
    // write section to a stringstream and read it back in, then check that the read section is the same as the original
    std::stringstream ss;
    section->write(ss);

    std::string section_str = ss.str();
    std::istringstream iss(section_str);
    Section* section2 = read_section(section->context(), section->allocator(), iss);
    std::stringstream ss2;
    section2->write(ss2);
    std::string read_section_str = ss2.str();
    if (section_str != read_section_str) {
        std::cerr << "sections->write() produced different results:\nEXPECTED:\n";
        std::cerr << section_str << "GOT:\n" << read_section_str;
    }
    unittest_assert(section_str == read_section_str);
}

int main() {
  metajit::LLVMCodeGen::initilize_llvm_jit();

  DiffTestSuite suite("tests/output/test_reader");

  suite.diff_test("bug_fold_lt_u").run([](Builder& builder, TestData& data) {
    Value* lt = builder.fold_lt_u(
      builder.build_const(Type::Int64, 1271752347623423UL),
      builder.build_const(Type::Int64, 2347782347823478UL));
    Block* true_block = builder.build_block();
    Block* false_block = builder.build_block();
    builder.fold_branch(lt, true_block, false_block);

    builder.move_to_end(true_block);
    data.output(lt);
    builder.build_exit();

    builder.move_to_end(false_block);
    data.output(lt);
    builder.build_exit();

    check_roundtrip(builder.section());
  });

  suite.diff_test("jump_many_args").run([](Builder& builder, TestData& data) {
    Value* cond = data.input(RandomRange(Type::Bool));
    Block* true_block = builder.build_block();
    Block* false_block = builder.build_block();
    std::vector<Arg*> args;
    for (size_t it = 0; it < 5; it++) {
        args.push_back(builder.alloc_arg(Type::Int64, it));
    }
    Block* merge_block = builder.build_block(args);
    builder.fold_branch(cond, true_block, false_block);

    builder.move_to_end(true_block);
    data.output(cond);

    std::vector<Value*> true_args;
    for (size_t it = 0; it < 5; it++) {
      Value* arg;
      if (!it) {
        arg = data.input(RandomRange(Type::Int64));
      } else {
        arg = builder.build_const(Type::Int64, it);
      }
      true_args.push_back(arg);
    }
    builder.build_jump(merge_block, true_args);

    builder.move_to_end(false_block);
    data.output(cond);
    builder.build_exit();

    builder.move_to_end(merge_block);
    builder.build_exit();

    check_roundtrip(builder.section());

  });
  return suite.finish();
}
