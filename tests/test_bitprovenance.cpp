#include "../bitprovenance.hpp"
#include "../../unittest.cpp/unittest.hpp"

using namespace metajit;

void check_provenance(const std::string& input, const std::string& expected,
                      const std::vector<size_t>& chain_indices = {0}) {
  Context context;
  Allocator allocator;
  std::istringstream input_stream(input), expected_stream(expected);
  Section* section = SectionReader<Builder>::read_section(context, allocator, input_stream);
  Section* target = SectionReader<Builder>::read_section(context, allocator, expected_stream);
  section->order_blocks(BlockOrdering::Dominator);
  std::vector<Block*> blocks;
  for (Block* block : *section) { blocks.push_back(block); }
  Chain chain;
  for (size_t index : chain_indices) { chain.add(blocks.at(index)); }
  BitProvenance::run(section, chain);
  unittest_assert(!section->verify(std::cerr));
  DeadCodeElim::run(section);
  section->autoname();
  target->autoname();
  std::ostringstream actual_text, expected_text;
  section->write(actual_text);
  target->write(expected_text);
  if (actual_text.str() != expected_text.str()) {
    std::cerr << "Expected:\n" << expected_text.str() << "Got:\n" << actual_text.str();
  }
  unittest_assert(actual_text.str() == expected_text.str());
  delete target;
  delete section;
}

void test_reverse_pair() {
  check_provenance(R"(section {
b0(%0: Ptr, %1: Int32):
  %2 = BitReverse %1
  %3 = ResizeU %2, type=Int64
  %4 = Shl %3, 31:Int64
  %5 = ShrS %4, 39:Int64
  %6 = ResizeU %5, type=Int32
  %7 = BitReverse %6
  Store %0, %7, aliasing=0, offset=0
  Exit
}
)", R"(section {
b0(%0: Ptr, %1: Int32):
  %2 = Shl %1, 8:Int32
  Store %0, %2, aliasing=0, offset=0
  Exit
}
)");
}

void test_reconstruction() {
  check_provenance(R"(section {
b0(%0: Ptr, %1: Int32):
  %2 = ResizeU %1, type=Int8
  %3 = ResizeX %2, type=Int32
  %4 = Shl %3, 24:Int32
  %5 = ShrU %4, 24:Int32
  %6 = And %1, 4294967040:Int32
  %7 = Or %5, %6
  Store %0, %7, aliasing=0, offset=0
  Exit
}
)", R"(section {
b0(%0: Ptr, %1: Int32):
  Store %0, %1, aliasing=0, offset=0
  Exit
}
)");
}

void test_sign_extension() {
  check_provenance(R"(section {
b0(%0: Ptr, %1: Int32):
  %2 = LtS %1, 0:Int32
  %3 = ResizeU %1, type=Int64
  %4 = ResizeU %2, type=Int64
  %5 = Shl %4, 32:Int64
  %6 = Or %3, %5
  %7 = Select %2, 18446744065119617024:Int64, 0:Int64
  %8 = Or %6, %7
  Store %0, %8, aliasing=0, offset=0
  Exit
}
)", R"(section {
b0(%0: Ptr, %1: Int32):
  %2 = ResizeS %1, type=Int64
  Store %0, %2, aliasing=0, offset=0
  Exit
}
)");
}

void test_sibling_reuse() {
  const std::string input = R"(section {
b0(%0: Ptr, %1: Int32, %2: Bool):
  Branch %2, true_block=b1, false_block=b2
b1:
  %3 = ShrU %1, 8:Int32
  Store %0, %3, aliasing=0, offset=0
  Exit
b2:
  %4 = ShrU %1, 8:Int32
  Store %0, %4, aliasing=0, offset=0
  Exit
}
)";
  check_provenance(input, input, {0, 1});
}

void test_combined_shift() {
  check_provenance(R"(section {
b0(%0: Ptr, %1: Int32):
  %2 = Shl %1, 8:Int32
  %3 = Shl %2, 4:Int32
  Store %0, %3, aliasing=0, offset=0
  Exit
}
)", R"(section {
b0(%0: Ptr, %1: Int32):
  %2 = Shl %1, 12:Int32
  Store %0, %2, aliasing=0, offset=0
  Exit
}
)");
}

void test_trace_chain() {
  check_provenance(R"(section {
b0(%0: Ptr, %1: Int32, %2: Bool):
  %3 = BitReverse %1
  %6 = Xor %1, 0:Int32
  Branch %2, true_block=b1, false_block=b2
b1:
  %4 = BitReverse %3
  Store %0, %4, aliasing=0, offset=0
  Exit
b2:
  %5 = BitReverse %3
  Store %0, %5, aliasing=0, offset=0
  Store %0, %6, aliasing=0, offset=4
  Exit
}
)", R"(section {
b0(%0: Ptr, %1: Int32, %2: Bool):
  %3 = BitReverse %1
  Branch %2, true_block=b1, false_block=b2
b1:
  Store %0, %1, aliasing=0, offset=0
  Exit
b2:
  %5 = BitReverse %3
  Store %0, %5, aliasing=0, offset=0
  Store %0, %1, aliasing=0, offset=4
  Exit
}
)", {0, 1});
}


void test_equivalence_map() {
  const std::string input = R"(section {
b0(%0: Ptr, %1: Int32):
  %2 = And %1, 255:Int32
  %3 = Or %2, 256:Int32
  %4 = And %3, 255:Int32
  Store %0, %2, aliasing=0, offset=0
  Store %0, %4, aliasing=0, offset=8
  Exit
}
)";
  check_provenance(input, R"(section {
b0(%0: Ptr, %1: Int32):
  %2 = And %1, 255:Int32
  Store %0, %2, aliasing=0, offset=0
  Store %0, %2, aliasing=0, offset=8
  Exit
}
)");
}

void test_candidate_map() {
  check_provenance(R"(section {
b0(%0: Ptr, %1: Int64):
  %2 = ShrU %1, 32:Int64
  %3 = ResizeU %2, type=Int32
  %4 = BitReverse %3
  %5 = ResizeU %4, type=Int64
  %6 = Shl %5, 31:Int64
  %7 = ShrS %6, 39:Int64
  %8 = ResizeU %7, type=Int32
  %9 = BitReverse %8
  Store %0, %9, aliasing=0, offset=0
  Exit
}
)", R"(section {
b0(%0: Ptr, %1: Int64):
  %2 = ShrU %1, 32:Int64
  %3 = ResizeU %2, type=Int32
  %4 = Shl %3, 8:Int32
  Store %0, %4, aliasing=0, offset=0
  Exit
}
)");
}

void test_first_four_keeps_equivalence() {
  Context context;
  Allocator allocator;
  Section section(context, allocator);
  Builder builder(&section);
  Block* block = builder.build_block({Type::Ptr, Type::Int32});
  builder.move_to_end(block);
  Value* x = block->arg(1);
  for (uint64_t shift = 1; shift <= 3; shift++) {
    builder.build_shl(x, builder.build_const(Type::Int32, shift));
  }
  Value* masked = builder.build_and(x, builder.build_const(Type::Int32, 255));
  Value* extended = builder.build_or(masked, builder.build_const(Type::Int32, 256));
  Value* repeated = builder.build_and(extended, builder.build_const(Type::Int32, 255));
  StoreInst* store = builder.build_store(block->arg(0), repeated, 0, 0);
  builder.build_exit();
  section.order_blocks(BlockOrdering::Natural);
  BitProvenance::Stats stats;
  BitProvenance::run(&section, Chain({block}), &stats);
  unittest_assert(store->value() == masked);
  unittest_assert(stats.equivalence_hits != 0);
  unittest_assert(!section.verify(std::cerr));
}

uint64_t provenance_random(uint64_t& state) {
  state = state * 6364136223846793005ull + 1442695040888963407ull;
  return state;
}

void test_random_execution() {
  uint64_t state = 1234567;
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    for (size_t sample = 0; sample < 20; sample++) {
      Context context;
      Allocator allocator;
      Section section(context, allocator);
      Builder builder(&section);
      Block* block = builder.build_block({Type::Ptr, type, type});
      builder.move_to_end(block);
      std::vector<Value*> values = {block->arg(1), block->arg(2)};
      for (size_t i = 0; i < 64; i++) {
        Value* a = values[provenance_random(state) % values.size()];
        Value* b = values[provenance_random(state) % values.size()];
        Const* shift = builder.build_const(type, provenance_random(state) % type_width(type));
        switch (provenance_random(state) % 10) {
          case 0: values.push_back(builder.build_and(a, b)); break;
          case 1: values.push_back(builder.build_or(a, b)); break;
          case 2: values.push_back(builder.build_xor(a, b)); break;
          case 3: values.push_back(builder.build_shl(a, shift)); break;
          case 4: values.push_back(builder.build_shr_u(a, shift)); break;
          case 5: values.push_back(builder.build_shr_s(a, shift)); break;
          case 6: values.push_back(builder.build_bit_reverse(a)); break;
          case 7: values.push_back(builder.build_add(a, b)); break;
          case 8: values.push_back(builder.build_resize_u(builder.build_resize_x(a, Type::Int64), type)); break;
          case 9: values.push_back(builder.build_select(builder.build_lt_s(a, builder.build_const(type, 0)), a, b)); break;
        }
      }
      for (size_t i = 0; i < 16; i++) { builder.build_store(block->arg(0), values[values.size() - 1 - i], 0, i * 8); }
      builder.build_exit();
      section.order_blocks(BlockOrdering::Natural);
      uint64_t before[16] = {}, after[16] = {};
      uint64_t a = provenance_random(state) & type_mask(type);
      uint64_t b = provenance_random(state) & type_mask(type);
      Interpreter original(&section, {Interpreter::Bits::constant(before), Interpreter::Bits::constant(type, a), Interpreter::Bits::constant(type, b)});
      unittest_assert(original.run() == Interpreter::Event::Exit);
      BitProvenance::run(&section, Chain({block}));
      unittest_assert(!section.verify(std::cerr));
      DeadCodeElim::run(&section);
      Interpreter optimized(&section, {Interpreter::Bits::constant(after), Interpreter::Bits::constant(type, a), Interpreter::Bits::constant(type, b)});
      unittest_assert(optimized.run() == Interpreter::Event::Exit);
      for (size_t i = 0; i < 16; i++) { unittest_assert(before[i] == after[i]); }
    }
  }
}

int main(int argc, char** argv) {
  unittest::Suite suite(argc, argv);
  suite.test("reverse pair").run(test_reverse_pair);
  suite.test("reconstruction").run(test_reconstruction);
  suite.test("sign extension").run(test_sign_extension);
  suite.test("sibling reuse").run(test_sibling_reuse);
  suite.test("trace chain").run(test_trace_chain);
  suite.test("random execution").run(test_random_execution);
  suite.test("combined shift").run(test_combined_shift);
  suite.test("equivalence map").run(test_equivalence_map);
  suite.test("candidate map").run(test_candidate_map);
  suite.test("first four keeps equivalence").run(test_first_four_keeps_equivalence);
  return suite.finish();
}
