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

void check_trace_simplify(const std::string& input,
                          std::initializer_list<size_t> chain_blocks,
                          const std::string& expected) {
  Context context;
  Allocator allocator;
  std::istringstream stream(input);
  std::unique_ptr<Section> section(SectionReader<>::read_section(context, allocator, stream));
  std::vector<Block*> blocks;
  for (Block* block : *section) {
    blocks.push_back(block);
  }
  Chain chain;
  for (size_t index : chain_blocks) {
    chain.add(blocks.at(index));
  }

  TestData data;
  for (Block* block : *section) {
    for (Inst* inst : *block) {
      if (dynmatch(LoadInst, load, inst)) {
        unittest_assert(load->ptr() == section->entry()->arg(0));
        unittest_assert(data.alloc_input(RandomRange(load->type())) == load->offset());
      } else if (dynmatch(StoreInst, store, inst)) {
        unittest_assert(store->ptr() == section->entry()->arg(0));
        unittest_assert(data.alloc_output(store->value()->type()) == store->offset());
      }
    }
  }

  unittest_assert(!section->verify(std::cout));
  Section original(context, allocator);
  Clone::run(section.get(), &original);
  SimplifyTrace::run(section.get(), &chain);
  std::stringstream ss;
  section->write(ss);
  if (ss.str() != expected) {
    std::cerr << "Expected:\n" << expected << "\n\nGot:\n" << ss.str() << std::endl;
  }
  unittest_assert(ss.str() == expected);

  unittest_assert(!section->verify(std::cout));
  check_opt_differential(&original, section.get(), data);
  check_codegen_differential("", section.get(), data);
}

int main(int argc, char** argv) {
  unittest::Suite suite(argc, argv);
  metajit::LLVMCodeGen::initilize_llvm_jit();

  suite.test("const_prop_branch").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int64, flags={}, aliasing=0, offset=8
  Branch %1, true_block=b1, false_block=b2
b1:
  %4 = Select %1, %2, 0:Int64
  Store %0, %4, aliasing=0, offset=16
  Jump block=b2
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int64, flags={}, aliasing=0, offset=8
  Branch %1, true_block=b1, false_block=b2
b1:
  %4 = Select 1:Bool, %2, 0:Int64
  Store %0, %2, aliasing=0, offset=16
  Jump block=b2
b2:
  Exit
}
)");
  });

  suite.test("const_prop_eq_backwards").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Eq %1, 42:Int8
  Branch %2, true_block=b1, false_block=b2
b1:
  %4 = Add %1, 17:Int8
  Store %0, %4, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Eq %1, 42:Int8
  Branch %2, true_block=b1, false_block=b2
b1:
  %4 = Add 42:Int8, 17:Int8
  Store %0, 59:Int8, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)");
  });

  suite.test("const_prop_resize_x_backwards").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = ResizeX %1, type=Bool
  Branch %2, true_block=b1, false_block=b2
b1:
  %4 = And %1, 1:Int8
  Store %0, %4, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = ResizeX %1, type=Bool
  Branch %2, true_block=b1, false_block=b2
b1:
  %4 = And %1, 1:Int8
  Store %0, 1:Int8, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)");
  });

  suite.test("backwards_with_intersect").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Or %1, 6:Int8
  %3 = ResizeX %2, type=Bool
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %2, 7:Int8
  Store %0, %5, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Or %1, 6:Int8
  %3 = ResizeX %2, type=Bool
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %2, 7:Int8
  Store %0, 7:Int8, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)");
  });

  suite.test("backwards_and").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = And %1, 7:Int8
  %3 = Eq %2, 7:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %1, 6:Int8
  Store %0, %5, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = And %1, 7:Int8
  %3 = Eq %2, 7:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %1, 6:Int8
  Store %0, 6:Int8, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)");
  });

  suite.test("backwards_select").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Select %1, 4:Int8, 7:Int8
  %3 = Eq %2, 4:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, %1, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Select %1, 4:Int8, 7:Int8
  %3 = Eq %2, 4:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, 1:Bool, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)");
  });

  suite.test("backwards_add").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Add %1, 1:Int8
  %3 = Eq %2, 4:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, %1, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Add %1, 1:Int8
  %3 = Eq %2, 4:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, 3:Int8, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)");
  });

  suite.test("backwards_shl").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Shl %1, 2:Int8
  %3 = Eq %2, 4:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %1, 15:Int8
  Store %0, %5, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Shl %1, 2:Int8
  %3 = Eq %2, 4:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %1, 15:Int8
  Store %0, 1:Int8, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)");
  });

  suite.test("backwards_resize_u").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = ResizeU %1, type=Int64
  %3 = Eq %2, 4:Int64
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, %1, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = ResizeU %1, type=Int64
  %3 = Eq %2, 4:Int64
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, 4:Int8, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)");
  });

  suite.test("backwards_xor").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Xor %1, 1:Bool
  %3 = Load %0, type=Int64, flags={}, aliasing=0, offset=8
  Branch %2, true_block=b1, false_block=b2
b1:
  Store %0, %1, aliasing=0, offset=16
  Jump block=b2
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Xor %1, 1:Bool
  %3 = Load %0, type=Int64, flags={}, aliasing=0, offset=8
  Branch %2, true_block=b1, false_block=b2
b1:
  Store %0, 0:Bool, aliasing=0, offset=16
  Jump block=b2
b2:
  Exit
}
)");
  });

  suite.test("eq_resizeu_bool").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = ResizeU %1, type=Int64
  %3 = Eq %2, 1:Int64
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, %1, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = ResizeU %1, type=Int64
  %3 = Eq %2, 1:Int64
  Branch %1, true_block=b1, false_block=b2
b1:
  Store %0, 1:Bool, aliasing=0, offset=1
  Jump block=b2
b2:
  Exit
}
)");
  });

  suite.test("completed_true_continuation").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  Branch %1, true_block=b1, false_block=b2
b1:
  Store %0, %1, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  Branch %1, true_block=b1, false_block=b2
b1:
  Store %0, 1:Bool, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)");
  });

  suite.test("completed_false_continuation").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  Branch %1, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Store %0, %1, aliasing=0, offset=1
  Exit
}
)", {0, 2}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  Branch %1, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Store %0, 0:Bool, aliasing=0, offset=1
  Exit
}
)");
  });

  suite.test("empty_chain").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  Branch %1, true_block=b1, false_block=b2
b1:
  Store %0, %1, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)";
    check_trace_simplify(ir, {}, ir);
  });

  suite.test("single_block_chain").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = And %1, 0:Bool
  Store %0, %2, aliasing=0, offset=1
  Exit
}
)", {0}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = And %1, 0:Bool
  Store %0, 0:Bool, aliasing=0, offset=1
  Exit
}
)");
  });

  suite.test("stop_at_chain_end").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Bool, flags={}, aliasing=0, offset=1
  Branch %1, true_block=b1, false_block=b2
b1:
  Branch %2, true_block=b3, false_block=b2
b2:
  Exit
b3:
  Store %0, %1, aliasing=0, offset=2
  Store %0, %2, aliasing=0, offset=3
  Jump block=b4
b4:
  Exit
}
)";
    check_trace_simplify(ir, {0, 1}, ir);
  });

  suite.test("multiple_guards").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Bool, flags={}, aliasing=0, offset=1
  Branch %1, true_block=b1, false_block=b2
b1:
  Branch %2, true_block=b3, false_block=b2
b2:
  Exit
b3:
  Store %0, %1, aliasing=0, offset=2
  Store %0, %2, aliasing=0, offset=3
  Jump block=b4
b4:
  Exit
}
)", {0, 1, 3}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Bool, flags={}, aliasing=0, offset=1
  Branch %1, true_block=b1, false_block=b2
b1:
  Branch %2, true_block=b3, false_block=b2
b2:
  Exit
b3:
  Store %0, 1:Bool, aliasing=0, offset=2
  Store %0, 1:Bool, aliasing=0, offset=3
  Jump block=b4
b4:
  Exit
}
)");
  });

  suite.test("continue_across_jump").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Bool, flags={}, aliasing=0, offset=1
  Branch %1, true_block=b1, false_block=b2
b1:
  Jump block=b3
b2:
  Exit
b3:
  Store %0, %1, aliasing=0, offset=2
  Store %0, %2, aliasing=0, offset=3
  Jump block=b4
b4:
  Exit
}
)", {0, 1, 3}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Bool, flags={}, aliasing=0, offset=1
  Branch %1, true_block=b1, false_block=b2
b1:
  Jump block=b3
b2:
  Exit
b3:
  Store %0, 1:Bool, aliasing=0, offset=2
  Store %0, %2, aliasing=0, offset=3
  Jump block=b4
b4:
  Exit
}
)");
  });

  suite.test("chain_head_with_same_branch_targets").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  Branch %1, true_block=b1, false_block=b1
b1:
  Store %0, %1, aliasing=0, offset=1
  Exit
}
)";
    check_trace_simplify(ir, {1}, ir);
  });

  suite.test("chain_head_with_other_predecessor").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  Branch %1, true_block=b1, false_block=b2
b1:
  Jump block=b2
b2:
  Store %0, %1, aliasing=0, offset=1
  Exit
}
)";
    check_trace_simplify(ir, {2}, ir);
  });

  suite.test("nonadjacent_chain_blocks").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  Branch %1, true_block=b1, false_block=b2
b1:
  Jump block=b3
b2:
  Exit
b3:
  %5 = And %1, 0:Bool
  Store %0, %5, aliasing=0, offset=1
  Exit
}
)", {0, 1, 3}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  Branch %1, true_block=b1, false_block=b2
b1:
  Jump block=b3
b2:
  Exit
b3:
  %5 = And 1:Bool, 0:Bool
  Store %0, 0:Bool, aliasing=0, offset=1
  Exit
}
)");
  });

  suite.test("backwards_or_left").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Or %1, 8:Int8
  %3 = Eq %2, 13:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %1, 7:Int8
  Store %0, %5, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Or %1, 8:Int8
  %3 = Eq %2, 13:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %1, 7:Int8
  Store %0, 5:Int8, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)");
  });

  suite.test("backwards_or_right").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Or 8:Int8, %1
  %3 = Eq %2, 13:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %1, 7:Int8
  Store %0, %5, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Or 8:Int8, %1
  %3 = Eq %2, 13:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %1, 7:Int8
  Store %0, 5:Int8, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)");
  });

  suite.test("backwards_sub_left_wrap").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Sub %1, 1:Int8
  %3 = Eq %2, 255:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, %1, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Sub %1, 1:Int8
  %3 = Eq %2, 255:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, 0:Int8, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)");
  });

  suite.test("backwards_sub_right_wrap").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Sub 1:Int8, %1
  %3 = Eq %2, 255:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, %1, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Sub 1:Int8, %1
  %3 = Eq %2, 255:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, 2:Int8, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)");
  });

  suite.test("backwards_or_false").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Bool, flags={}, aliasing=0, offset=1
  %3 = Or %1, %2
  Branch %3, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Store %0, %1, aliasing=0, offset=2
  Store %0, %2, aliasing=0, offset=3
  Exit
}
)", {0, 2}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Bool, flags={}, aliasing=0, offset=1
  %3 = Or %1, %2
  Branch %3, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Store %0, 0:Bool, aliasing=0, offset=2
  Store %0, 0:Bool, aliasing=0, offset=3
  Exit
}
)");
  });

  suite.test("backwards_sub_partial_left").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = And %2, 254:Int8
  %4 = Sub %1, %3
  %5 = ResizeX %4, type=Bool
  Branch %5, true_block=b1, false_block=b2
b1:
  %7 = And %1, 1:Int8
  Store %0, %7, aliasing=0, offset=2
  Exit
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = And %2, 254:Int8
  %4 = Sub %1, %3
  %5 = ResizeX %4, type=Bool
  Branch %5, true_block=b1, false_block=b2
b1:
  %7 = And %1, 1:Int8
  Store %0, 1:Int8, aliasing=0, offset=2
  Exit
b2:
  Exit
}
)");
  });

  suite.test("backwards_sub_partial_right").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = And %2, 254:Int8
  %4 = Sub %3, %1
  %5 = ResizeX %4, type=Bool
  Branch %5, true_block=b1, false_block=b2
b1:
  %7 = And %1, 1:Int8
  Store %0, %7, aliasing=0, offset=2
  Exit
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = And %2, 254:Int8
  %4 = Sub %3, %1
  %5 = ResizeX %4, type=Bool
  Branch %5, true_block=b1, false_block=b2
b1:
  %7 = And %1, 1:Int8
  Store %0, 1:Int8, aliasing=0, offset=2
  Exit
b2:
  Exit
}
)");
  });

  suite.test("backwards_eq_constant_left").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Eq 42:Int8, %1
  Branch %2, true_block=b1, false_block=b2
b1:
  Store %0, %1, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Eq 42:Int8, %1
  Branch %2, true_block=b1, false_block=b2
b1:
  Store %0, 42:Int8, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)");
  });

  suite.test("backwards_eq_partial_intersection").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = And %1, 254:Int8
  %4 = Or %2, 2:Int8
  %5 = Eq %3, %4
  Branch %5, true_block=b1, false_block=b2
b1:
  %7 = And %1, 2:Int8
  %8 = And %2, 1:Int8
  Store %0, %7, aliasing=0, offset=2
  Store %0, %8, aliasing=0, offset=3
  Exit
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = And %1, 254:Int8
  %4 = Or %2, 2:Int8
  %5 = Eq %3, %4
  Branch %5, true_block=b1, false_block=b2
b1:
  %7 = And %1, 2:Int8
  %8 = And %2, 1:Int8
  Store %0, 2:Int8, aliasing=0, offset=2
  Store %0, 0:Int8, aliasing=0, offset=3
  Exit
b2:
  Exit
}
)");
  });

  suite.test("backwards_eq_false_keeps_partial_bits").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = And %1, 254:Int8
  %4 = Or %2, 2:Int8
  %5 = Eq %3, %4
  Branch %5, true_block=b1, false_block=b2
b1:
  Exit
b2:
  %8 = And %1, 2:Int8
  %9 = And %2, 1:Int8
  Store %0, %8, aliasing=0, offset=2
  Store %0, %9, aliasing=0, offset=3
  Exit
}
)";
    check_trace_simplify(ir, {0, 2}, ir);
  });

  suite.test("backwards_eq_intersection_becomes_constant").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = And %1, 15:Int8
  %4 = And %2, 240:Int8
  %5 = Eq %3, %4
  Branch %5, true_block=b1, false_block=b2
b1:
  Store %0, %3, aliasing=0, offset=2
  Store %0, %4, aliasing=0, offset=3
  Exit
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = And %1, 15:Int8
  %4 = And %2, 240:Int8
  %5 = Eq %3, %4
  Branch %5, true_block=b1, false_block=b2
b1:
  Store %0, 0:Int8, aliasing=0, offset=2
  Store %0, 0:Int8, aliasing=0, offset=3
  Exit
b2:
  Exit
}
)");
  });

  suite.test("backwards_shr_u_keeps_discarded_bits_unknown").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = ShrU %1, 3:Int8
  %3 = Eq %2, 22:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %1, 248:Int8
  %6 = And %1, 7:Int8
  Store %0, %5, aliasing=0, offset=1
  Store %0, %6, aliasing=0, offset=2
  Exit
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = ShrU %1, 3:Int8
  %3 = Eq %2, 22:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %1, 248:Int8
  %6 = And %1, 7:Int8
  Store %0, 176:Int8, aliasing=0, offset=1
  Store %0, %6, aliasing=0, offset=2
  Exit
b2:
  Exit
}
)");
  });

  suite.test("backwards_shr_u_partial").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = ShrU %1, 3:Int8
  %3 = ResizeX %2, type=Bool
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %1, 8:Int8
  Store %0, %5, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = ShrU %1, 3:Int8
  %3 = ResizeX %2, type=Bool
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %1, 8:Int8
  Store %0, 8:Int8, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)");
  });

  suite.test("backwards_shr_u_high_bit").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int64, flags={}, aliasing=0, offset=0
  %2 = ShrU %1, 63:Int64
  %3 = ResizeX %2, type=Bool
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %1, 9223372036854775808:Int64
  Store %0, %5, aliasing=0, offset=8
  Exit
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int64, flags={}, aliasing=0, offset=0
  %2 = ShrU %1, 63:Int64
  %3 = ResizeX %2, type=Bool
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = And %1, 9223372036854775808:Int64
  Store %0, 9223372036854775808:Int64, aliasing=0, offset=8
  Exit
b2:
  Exit
}
)");
  });

  suite.test("backwards_shr_u_zero").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = ShrU %1, 0:Int8
  %3 = Eq %2, 42:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, %1, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = ShrU %1, 0:Int8
  %3 = Eq %2, 42:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, 42:Int8, aliasing=0, offset=1
  Exit
b2:
  Exit
}
)");
  });

  suite.test("backwards_shr_u_variable_shift").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = And %2, 7:Int8
  %4 = ShrU %1, %3
  %5 = Eq %4, 1:Int8
  Branch %5, true_block=b1, false_block=b2
b1:
  Store %0, %1, aliasing=0, offset=2
  Exit
b2:
  Exit
}
)";
    check_trace_simplify(ir, {0, 1}, ir);
  });

  suite.test("substitute_loop_arguments_after_guard").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  Jump %1, %1, block=b1
b1(%3: Int8, %4: Int8):
  %5 = Eq %3, 42:Int8
  Branch %5, true_block=b2, false_block=b3
b2:
  Store %0, %3, aliasing=0, offset=1
  Jump 0:Int8, %3, block=b1
b3:
  Exit
}
)", {1, 2}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  Jump %1, %1, block=b1
b1(%3: Int8, %4: Int8):
  %5 = Eq %3, 42:Int8
  Branch %5, true_block=b2, false_block=b3
b2:
  Store %0, 42:Int8, aliasing=0, offset=1
  Jump 0:Int8, 42:Int8, block=b1
b3:
  Exit
}
)");
  });

  suite.test("substitute_alias_after_guard").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Select 1:Bool, %1, 0:Int8
  %3 = Eq %1, 42:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = Add %2, 1:Int8
  Store %0, %5, aliasing=0, offset=1
  Store %0, %2, aliasing=0, offset=2
  Exit
b2:
  Exit
}
)", {0, 1}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Select 1:Bool, %1, 0:Int8
  %3 = Eq %1, 42:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  %5 = Add 42:Int8, 1:Int8
  Store %0, 43:Int8, aliasing=0, offset=1
  Store %0, 42:Int8, aliasing=0, offset=2
  Exit
b2:
  Exit
}
)");
  });

  suite.test("backwards_select_known_true").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = Load %0, type=Int8, flags={}, aliasing=0, offset=2
  %4 = Select %1, %2, %3
  %5 = Eq %4, 42:Int8
  Branch %1, true_block=b1, false_block=b2
b1:
  Branch %5, true_block=b3, false_block=b2
b2:
  Exit
b3:
  Store %0, %2, aliasing=0, offset=3
  Store %0, %3, aliasing=0, offset=4
  Exit
}
)", {0, 1, 3}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = Load %0, type=Int8, flags={}, aliasing=0, offset=2
  %4 = Select %1, %2, %3
  %5 = Eq %4, 42:Int8
  Branch %1, true_block=b1, false_block=b2
b1:
  Branch %5, true_block=b3, false_block=b2
b2:
  Exit
b3:
  Store %0, 42:Int8, aliasing=0, offset=3
  Store %0, %3, aliasing=0, offset=4
  Exit
}
)");
  });

  suite.test("backwards_select_known_true_partial").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = Load %0, type=Int8, flags={}, aliasing=0, offset=2
  %4 = Select %1, %2, %3
  %5 = ResizeX %4, type=Bool
  Branch %1, true_block=b1, false_block=b2
b1:
  Branch %5, true_block=b3, false_block=b2
b2:
  Exit
b3:
  %9 = And %2, 1:Int8
  %10 = And %3, 1:Int8
  Store %0, %9, aliasing=0, offset=3
  Store %0, %10, aliasing=0, offset=4
  Exit
}
)", {0, 1, 3}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = Load %0, type=Int8, flags={}, aliasing=0, offset=2
  %4 = Select %1, %2, %3
  %5 = ResizeX %4, type=Bool
  Branch %1, true_block=b1, false_block=b2
b1:
  Branch %5, true_block=b3, false_block=b2
b2:
  Exit
b3:
  %9 = And %2, 1:Int8
  %10 = And %3, 1:Int8
  Store %0, 1:Int8, aliasing=0, offset=3
  Store %0, %10, aliasing=0, offset=4
  Exit
}
)");
  });

  suite.test("backwards_select_known_false").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = Load %0, type=Int8, flags={}, aliasing=0, offset=2
  %4 = Select %1, %2, %3
  %5 = Eq %4, 42:Int8
  Branch %1, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Branch %5, true_block=b3, false_block=b4
b3:
  Store %0, %2, aliasing=0, offset=3
  Store %0, %3, aliasing=0, offset=4
  Exit
b4:
  Exit
}
)", {0, 2, 3}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = Load %0, type=Int8, flags={}, aliasing=0, offset=2
  %4 = Select %1, %2, %3
  %5 = Eq %4, 42:Int8
  Branch %1, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Branch %5, true_block=b3, false_block=b4
b3:
  Store %0, %2, aliasing=0, offset=3
  Store %0, 42:Int8, aliasing=0, offset=4
  Exit
b4:
  Exit
}
)");
  });

  suite.test("backwards_select_known_false_partial").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = Load %0, type=Int8, flags={}, aliasing=0, offset=2
  %4 = Select %1, %2, %3
  %5 = ResizeX %4, type=Bool
  Branch %1, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Branch %5, true_block=b3, false_block=b4
b3:
  %9 = And %2, 1:Int8
  %10 = And %3, 1:Int8
  Store %0, %9, aliasing=0, offset=3
  Store %0, %10, aliasing=0, offset=4
  Exit
b4:
  Exit
}
)", {0, 2, 3}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = Load %0, type=Int8, flags={}, aliasing=0, offset=2
  %4 = Select %1, %2, %3
  %5 = ResizeX %4, type=Bool
  Branch %1, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Branch %5, true_block=b3, false_block=b4
b3:
  %9 = And %2, 1:Int8
  %10 = And %3, 1:Int8
  Store %0, %9, aliasing=0, offset=3
  Store %0, 1:Int8, aliasing=0, offset=4
  Exit
b4:
  Exit
}
)");
  });

  suite.test("backwards_eq_false_bool_0_right").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Eq %1, 0:Bool
  Branch %2, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Store %0, %1, aliasing=0, offset=1
  Exit
}
)", {0, 2}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Eq %1, 0:Bool
  Branch %2, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Store %0, 1:Bool, aliasing=0, offset=1
  Exit
}
)");
  });

  suite.test("backwards_eq_false_bool_0_left").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Eq 0:Bool, %1
  Branch %2, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Store %0, %1, aliasing=0, offset=1
  Exit
}
)", {0, 2}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Eq 0:Bool, %1
  Branch %2, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Store %0, 1:Bool, aliasing=0, offset=1
  Exit
}
)");
  });

  suite.test("backwards_eq_false_bool_1_right").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Eq %1, 1:Bool
  Branch %2, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Store %0, %1, aliasing=0, offset=1
  Exit
}
)", {0, 2}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Eq %1, 1:Bool
  Branch %2, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Store %0, 0:Bool, aliasing=0, offset=1
  Exit
}
)");
  });

  suite.test("backwards_eq_false_bool_1_left").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Eq 1:Bool, %1
  Branch %2, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Store %0, %1, aliasing=0, offset=1
  Exit
}
)", {0, 2}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Eq 1:Bool, %1
  Branch %2, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Store %0, 0:Bool, aliasing=0, offset=1
  Exit
}
)");
  });

  suite.test("backwards_eq_false_bool_guard_known_operand").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Bool, flags={}, aliasing=0, offset=1
  %3 = Eq %1, %2
  Branch %1, true_block=b1, false_block=b2
b1:
  Branch %3, true_block=b2, false_block=b3
b2:
  Exit
b3:
  Store %0, %1, aliasing=0, offset=2
  Store %0, %2, aliasing=0, offset=3
  Exit
}
)", {0, 1, 3}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Bool, flags={}, aliasing=0, offset=1
  %3 = Eq %1, %2
  Branch %1, true_block=b1, false_block=b2
b1:
  Branch %3, true_block=b2, false_block=b3
b2:
  Exit
b3:
  Store %0, 1:Bool, aliasing=0, offset=2
  Store %0, 0:Bool, aliasing=0, offset=3
  Exit
}
)");
  });

  suite.test("backwards_eq_false_bool_unknown_operands").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Bool, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Bool, flags={}, aliasing=0, offset=1
  %3 = Eq %1, %2
  Branch %3, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Store %0, %1, aliasing=0, offset=2
  Store %0, %2, aliasing=0, offset=3
  Exit
}
)";
    check_trace_simplify(ir, {0, 2}, ir);
  });

  suite.test("backwards_eq_false_integer_zero").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Eq %1, 0:Int8
  Branch %2, true_block=b1, false_block=b2
b1:
  Exit
b2:
  Store %0, %1, aliasing=0, offset=1
  Exit
}
)";
    check_trace_simplify(ir, {0, 2}, ir);
  });

  suite.test("and_reassociate_right_right").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = And %1, 15:Int8
  %3 = And %2, 6:Int8
  Store %0, %2, aliasing=0, offset=1
  Store %0, %3, aliasing=0, offset=2
  Exit
}
)", {0}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = And %1, 15:Int8
  %3 = And %1, 6:Int8
  Store %0, %2, aliasing=0, offset=1
  Store %0, %3, aliasing=0, offset=2
  Exit
}
)");
  });

  suite.test("and_reassociate_right_left").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = And %1, 15:Int8
  %3 = And 6:Int8, %2
  Store %0, %2, aliasing=0, offset=1
  Store %0, %3, aliasing=0, offset=2
  Exit
}
)", {0}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = And %1, 15:Int8
  %3 = And %1, 6:Int8
  Store %0, %2, aliasing=0, offset=1
  Store %0, %3, aliasing=0, offset=2
  Exit
}
)");
  });

  suite.test("and_reassociate_left_right").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = And 15:Int8, %1
  %3 = And %2, 6:Int8
  Store %0, %2, aliasing=0, offset=1
  Store %0, %3, aliasing=0, offset=2
  Exit
}
)", {0}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = And 15:Int8, %1
  %3 = And %1, 6:Int8
  Store %0, %2, aliasing=0, offset=1
  Store %0, %3, aliasing=0, offset=2
  Exit
}
)");
  });

  suite.test("and_reassociate_left_left").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = And 15:Int8, %1
  %3 = And 6:Int8, %2
  Store %0, %2, aliasing=0, offset=1
  Store %0, %3, aliasing=0, offset=2
  Exit
}
)", {0}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = And 15:Int8, %1
  %3 = And %1, 6:Int8
  Store %0, %2, aliasing=0, offset=1
  Store %0, %3, aliasing=0, offset=2
  Exit
}
)");
  });

  suite.test("and_reassociate_deep").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = And %1, 63:Int8
  %3 = And %2, 30:Int8
  %4 = And %3, 21:Int8
  Store %0, %4, aliasing=0, offset=1
  Exit
}
)", {0}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = And %1, 63:Int8
  %3 = And %1, 30:Int8
  %4 = And %1, 20:Int8
  Store %0, %4, aliasing=0, offset=1
  Exit
}
)");
  });

  suite.test("and_reassociate_disjoint_masks").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = And %1, 15:Int8
  %3 = And %2, 240:Int8
  Store %0, %3, aliasing=0, offset=1
  Exit
}
)", {0}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = And %1, 15:Int8
  %3 = And %1, 0:Int8
  Store %0, 0:Int8, aliasing=0, offset=1
  Exit
}
)");
  });

  suite.test("and_reassociate_high_bit").run([]() {
    check_trace_simplify(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int64, flags={}, aliasing=0, offset=0
  %2 = And %1, 9223372036854775823:Int64
  %3 = And %2, 9223372036854775814:Int64
  Store %0, %3, aliasing=0, offset=8
  Exit
}
)", {0}, R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int64, flags={}, aliasing=0, offset=0
  %2 = And %1, 9223372036854775823:Int64
  %3 = And %1, 9223372036854775814:Int64
  Store %0, %3, aliasing=0, offset=8
  Exit
}
)");
  });

  suite.test("and_reassociate_requires_inner_constant").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Load %0, type=Int8, flags={}, aliasing=0, offset=1
  %3 = And %1, %2
  %4 = And %3, 6:Int8
  Store %0, %4, aliasing=0, offset=2
  Exit
}
)";
    check_trace_simplify(ir, {0}, ir);
  });

  suite.test("deep_backwards_propagation").run([]() {
    Context context;
    Allocator allocator;
    Section section(context, allocator);
    Builder builder(&section);
    Block* entry = builder.build_block({Type::Ptr, Type::Int64});
    builder.move_to_end(entry);
    Value* value = entry->arg(1);
    const uint64_t depth = 100000;
    for (uint64_t index = 0; index < depth; index++) {
      value = builder.build_add(value, builder.build_const(Type::Int64, 1));
    }
    Value* cond = builder.build_eq(value, builder.build_const(Type::Int64, 42));
    Block* success = builder.build_block();
    Block* failure = builder.build_block();
    builder.build_branch(cond, success, failure);
    builder.move_to_end(success);
    StoreInst* store = builder.build_store(entry->arg(0), entry->arg(1), AliasingGroup(0), 0);
    builder.build_exit();
    builder.move_to_end(failure);
    builder.build_exit();

    Chain chain({entry, success});
    SimplifyTrace::run(&section, &chain);
    Const* result = dynamic_cast<Const*>(store->value());
    unittest_assert(result);
    unittest_assert(result->value() == uint64_t(42) - depth);
  });

  return suite.finish();
}
