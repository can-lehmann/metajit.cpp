#include "diff.hpp"

using namespace metajit;
using namespace metajit::test;

Section* read_sinking_section(Context& context, Allocator& allocator, const std::string& input,
                              bool preserve_order = false) {
  std::istringstream stream(input);
  Section* section = SectionReader<>::read_section(context, allocator, stream);
  if (preserve_order) {
    section->set_ordering(BlockOrdering::Dominator);
  } else {
    OrderBlocks::run(section, BlockOrdering::Natural);
  }
  unittest_assert(!section->verify(std::cout));
  return section;
}

void check_sinking(const std::string& input, const std::string& expected, bool preserve_order = false) {
  Context context;
  Allocator allocator;
  Section* section = read_sinking_section(context, allocator, input, preserve_order);
  Section* expected_section = read_sinking_section(context, allocator, expected, preserve_order);
  Section original(context, allocator);
  Clone::run(section, &original);
  CodeSinking::run(section);
  unittest_assert(!section->verify(std::cout));
  std::stringstream result;
  section->write(result);
  std::stringstream expected_result;
  expected_section->write(expected_result);
  if (result.str() != expected_result.str()) {
    std::cerr << "Expected:\n" << expected_result.str() << "\nGot:\n" << result.str();
  }
  unittest_assert(result.str() == expected_result.str());
  CodeSinking::run(section);
  std::stringstream twice;
  section->write(twice);
  unittest_assert(result.str() == twice.str());
  for (size_t input = 0; input < 512; input++) {
    uint8_t before[16] = {uint8_t(input), uint8_t(input >> 8)};
    uint8_t after[16];
    std::copy(before, before + 16, after);
    Interpreter before_interp(&original, {Interpreter::Bits::constant(before)});
    Interpreter after_interp(section, {Interpreter::Bits::constant(after)});
    unittest_assert(before_interp.run() == Interpreter::Event::Exit);
    unittest_assert(after_interp.run() == Interpreter::Event::Exit);
    unittest_assert(std::memcmp(before, after, 16) == 0);
  }
  delete expected_section;
  delete section;
}

void check_sinking_reentry() {
  Context context;
  Allocator allocator;
  Section section(context, allocator);
  Builder builder(&section);
  Block* entry = builder.build_block({Type::Ptr});
  Block* second = builder.build_block();
  Block* compressed = builder.build_block();
  Block* uncompressed = builder.build_block();
  Block* illegal = builder.build_block();
  builder.move_to_end(entry);
  LoadInst* input = builder.build_load(entry->arg(0), Type::Int8, LoadFlags::None, 0, 0);
  AddInst* added = builder.build_add(input, builder.build_const(Type::Int8, 1));
  ShlInst* immediate = builder.build_shl(added, builder.build_const(Type::Int8, 2));
  Value* low = builder.build_and(input, builder.build_const(Type::Int8, 3));
  Value* first_cond = builder.build_eq(low, builder.build_const(Type::Int8, 3));
  builder.build_store(entry->arg(0), input, 0, 9);
  BranchInst* first_guard = builder.build_branch(first_cond, uncompressed, second);
  builder.move_to_end(second);
  Value* second_cond = builder.build_eq(input, builder.build_const(Type::Int8, 0));
  builder.build_branch(second_cond, illegal, compressed);
  builder.move_to_end(compressed);
  builder.build_store(entry->arg(0), immediate, 0, 8);
  builder.build_exit();
  builder.move_to_end(uncompressed);
  builder.build_store(entry->arg(0), input, 0, 8);
  builder.build_exit();
  builder.move_to_end(illegal);
  builder.build_store(entry->arg(0), builder.build_const(Type::Int8, 0), 0, 8);
  builder.build_exit();
  OrderBlocks::run(&section, BlockOrdering::Topological);
  unittest_assert(!section.verify(std::cout));
  BindingTimeGroups before_binding_times(&section);
  ReentryClosures before_closures(&section, before_binding_times);
  unittest_assert(before_closures.is_captured(immediate));
  CodeSinking::run(&section);
  unittest_assert(!section.verify(std::cout));
  BindingTimeGroups binding_times(&section);
  ReentryClosures closures(&section, binding_times);
  unittest_assert(!closures.is_captured(added));
  unittest_assert(!closures.is_captured(immediate));
  unittest_assert(closures.is_captured(input));
  Section reentry(context, allocator);
  Clone clone(&section, &reentry);
  ReentryClosures cloned_closures(closures, clone);
  SliceReentryClosures::run(&reentry, cloned_closures);
  unittest_assert(!reentry.verify(std::cout));
  for (size_t value = 0; value < 256; value++) {
    uint8_t data[16] = {uint8_t(value)};
    Interpreter normal(&section, {Interpreter::Bits::constant(data)});
    while (normal.inst() != first_guard) {
      unittest_assert(normal.step() != Interpreter::Event::Exit);
    }
    ReentryClosures::Closure& closure = closures.at(first_guard);
    unittest_assert(!closure.reuse);
    std::vector<uint8_t> captures(closures.max_size(), 0);
    std::memcpy(captures.data(), &closure.id, sizeof(closure.id));
    for (const ReentryClosures::Capture& capture : closure.captures) {
      normal.at(capture.value).store(captures.data() + capture.offset);
    }
    Interpreter resumed(&reentry, {Interpreter::Bits::constant(captures.data())});
    unittest_assert(resumed.run() == Interpreter::Event::Exit);
    uint8_t expected = (value & 3) == 3 ? value : value == 0 ? 0 : (value + 1) << 2;
    unittest_assert(data[8] == expected);
  }
}

int main(int argc, char** argv) {
  unittest::Suite suite(argc, argv);
  suite.test("decoder reentry captures input instead of intermediates").run(check_sinking_reentry);
  suite.test("expression chain past a write").run([]() {
    check_sinking(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Add %1, 1:Int8
  %3 = Shl %2, 2:Int8
  %4 = Load %0, type=Bool, flags={}, aliasing=0, offset=1
  Store %0, 99:Int8, aliasing=0, offset=0
  Branch %4, true_block=b1, false_block=b2
b1:
  Store %0, %3, aliasing=0, offset=8
  Exit
b2:
  Exit
}
)", R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %4 = Load %0, type=Bool, flags={}, aliasing=0, offset=1
  Store %0, 99:Int8, aliasing=0, offset=0
  Branch %4, true_block=b1, false_block=b2
b1:
  %2 = Add %1, 1:Int8
  %3 = Shl %2, 2:Int8
  Store %0, %3, aliasing=0, offset=8
  Exit
b2:
  Exit
}
)");
  });

  suite.test("common dominator of multiple uses").run([]() {
    check_sinking(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Add %1, 1:Int8
  %3 = Load %0, type=Bool, flags={}, aliasing=0, offset=1
  Branch %3, true_block=b1, false_block=b4
b1:
  %5 = Eq %1, 42:Int8
  Branch %5, true_block=b2, false_block=b3
b2:
  Store %0, %2, aliasing=0, offset=8
  Exit
b3:
  Store %0, %2, aliasing=0, offset=9
  Exit
b4:
  Exit
}
)", R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %3 = Load %0, type=Bool, flags={}, aliasing=0, offset=1
  Branch %3, true_block=b1, false_block=b4
b1:
  %2 = Add %1, 1:Int8
  %5 = Eq %1, 42:Int8
  Branch %5, true_block=b2, false_block=b3
b2:
  Store %0, %2, aliasing=0, offset=8
  Exit
b3:
  Store %0, %2, aliasing=0, offset=9
  Exit
b4:
  Exit
}
)");
  });

  suite.test("both successors use the value").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Add %1, 1:Int8
  %3 = Load %0, type=Bool, flags={}, aliasing=0, offset=1
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, %2, aliasing=0, offset=8
  Exit
b2:
  Store %0, %2, aliasing=0, offset=9
  Exit
}
)";
    check_sinking(ir, ir);
  });

  suite.test("jump arguments are predecessor uses").run([]() {
    check_sinking(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Add %1, 1:Int8
  %3 = Load %0, type=Bool, flags={}, aliasing=0, offset=1
  Branch %3, true_block=b1, false_block=b2
b1:
  Jump %2, block=b3
b2:
  Jump 0:Int8, block=b3
b3(%7: Int8):
  Store %0, %7, aliasing=0, offset=8
  Exit
}
)", R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %3 = Load %0, type=Bool, flags={}, aliasing=0, offset=1
  Branch %3, true_block=b1, false_block=b2
b1:
  %2 = Add %1, 1:Int8
  Jump %2, block=b3
b2:
  Jump 0:Int8, block=b3
b3(%7: Int8):
  Store %0, %7, aliasing=0, offset=8
  Exit
}
)");
  });

  suite.test("loop destination stays in preheader").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Add %1, 1:Int8
  Jump 3:Int8, block=b1
b1(%4: Int8):
  Store %0, %2, aliasing=0, offset=8
  %6 = Sub %4, 1:Int8
  %7 = Eq %6, 0:Int8
  Branch %7, true_block=b3, false_block=b2
b2:
  Jump %6, block=b1
b3:
  Exit
}
)";
    check_sinking(ir, ir);
  });

  suite.test("branch condition stays before guard").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Add %1, 1:Int8
  %3 = Eq %2, 42:Int8
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, %2, aliasing=0, offset=8
  Exit
b2:
  Exit
}
)";
    check_sinking(ir, ir);
  });

  suite.test("pinned operations").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Freeze %1
  %3 = Promote %2
  %4 = AssumeConst %3
  %5 = Alloca 8:Int64, align=8
  %6 = Load %0, type=Bool, flags={}, aliasing=0, offset=1
  Branch %6, true_block=b1, false_block=b2
b1:
  Store %5, %4, aliasing=0, offset=0
  Store %0, %4, aliasing=0, offset=8
  Exit
b2:
  Exit
}
)";
    check_sinking(ir, ir);
  });

  suite.test("shared side exit").run([]() {
    check_sinking(R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Add %1, 1:Int8
  %3 = Eq %1, 0:Int8
  Branch %3, true_block=b3, false_block=b1
b1:
  %5 = Eq %1, 1:Int8
  Branch %5, true_block=b3, false_block=b2
b2:
  Store %0, 0:Int8, aliasing=0, offset=8
  Exit
b3:
  Store %0, %2, aliasing=0, offset=8
  Exit
}
)", R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %3 = Eq %1, 0:Int8
  Branch %3, true_block=b3, false_block=b1
b1:
  %5 = Eq %1, 1:Int8
  Branch %5, true_block=b3, false_block=b2
b2:
  Store %0, 0:Int8, aliasing=0, offset=8
  Exit
b3:
  %2 = Add %1, 1:Int8
  Store %0, %2, aliasing=0, offset=8
  Exit
}
)");
  });

  suite.test("unreachable source stays in place").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  Exit
b1:
  %2 = Add %1, 1:Int8
  Jump block=b2
b2:
  Store %0, %2, aliasing=0, offset=8
  Exit
}
)";
    check_sinking(ir, ir, true);
  });

  suite.test("unreachable use prevents sinking").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Add %1, 1:Int8
  %3 = Load %0, type=Bool, flags={}, aliasing=0, offset=1
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, %2, aliasing=0, offset=8
  Exit
b2:
  Exit
b3:
  Store %0, %2, aliasing=0, offset=9
  Exit
}
)";
    check_sinking(ir, ir, true);
  });

  suite.test("nested loops retain computations outside deeper loops").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Add %1, 1:Int8
  Jump 2:Int8, block=b1
b1(%4: Int8):
  %5 = Add %1, %4
  Jump 3:Int8, block=b2
b2(%7: Int8):
  Store %0, %2, aliasing=0, offset=8
  Store %0, %5, aliasing=0, offset=9
  %10 = Sub %7, 1:Int8
  %11 = Eq %10, 0:Int8
  Branch %11, true_block=b4, false_block=b3
b3:
  Jump %10, block=b2
b4:
  %14 = Sub %4, 1:Int8
  %15 = Eq %14, 0:Int8
  Branch %15, true_block=b6, false_block=b5
b5:
  Jump %14, block=b1
b6:
  Exit
}
)";
    check_sinking(ir, ir);
  });

  suite.test("dominator ordering with an acyclic backward edge").run([]() {
    const std::string ir = R"(section {
b0(%0: Ptr):
  %1 = Load %0, type=Int8, flags={}, aliasing=0, offset=0
  %2 = Add %1, 1:Int8
  %3 = Load %0, type=Bool, flags={}, aliasing=0, offset=1
  Branch %3, true_block=b1, false_block=b2
b1:
  Store %0, %2, aliasing=0, offset=8
  Exit
b2:
  Jump block=b1
}
)";
    check_sinking(ir, ir, true);
  });

  return suite.finish();
}
