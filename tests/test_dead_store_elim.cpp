#include "../jitir.hpp"
#include "../../unittest.cpp/unittest.hpp"

using namespace metajit;

void check_dead_stores(const std::string& body, const std::vector<uint64_t>& expected) {
  Context context;
  Allocator allocator;
  std::istringstream input("section {\nb0(%0: Ptr, %1: Ptr):\n" + body + "  Exit\n}\n");
  Section* section = SectionReader<Builder>::read_section(context, allocator, input);
  section->autoname();
  unittest_assert(!section->verify(std::cout));
  DeadStoreElim::run(section);
  unittest_assert(!section->verify(std::cout));
  std::vector<uint64_t> stored_values;
  for (Block* block : *section) {
    for (Inst* inst : *block) {
      if (dynmatch(StoreInst, store, inst)) {
        stored_values.push_back(static_cast<Const*>(store->value())->value());
      }
    }
  }
  if (stored_values != expected) section->write(std::cerr);
  unittest_assert(stored_values == expected);
  delete section;
}

void test_dead_store_exact_overwrite() {
  const std::string first = "  Store %0, 1:Int32, flags={}, aliasing=3, offset=0\n";
  const std::string last = "  Store %0, 2:Int32, flags={}, aliasing=3, offset=0\n";
  check_dead_stores(first + last, {2});
  check_dead_stores(first + last + "  Store %0, 3:Int32, flags={}, aliasing=3, offset=0\n", {3});
  check_dead_stores(first + "  Store %0, 3:Int32, flags={}, aliasing=3, offset=4\n" +
                    "  %2 = Load %0, type=Int8, flags={}, aliasing=3, offset=4\n" +
                    last + "  Store %0, 4:Int32, flags={}, aliasing=3, offset=4\n", {3, 2, 4});
  check_dead_stores(first + "  %2 = Load %1, type=Int32, flags={}, aliasing=4, offset=0\n" + last, {2});
  check_dead_stores(first + "  Store %1, 2:Int32, flags={}, aliasing=3, offset=0\n", {1, 2});
  check_dead_stores(first + "  Store %0, 2:Int32, flags={}, aliasing=3, offset=1\n", {1, 2});
  check_dead_stores(first + "  Store %0, 2:Int8, flags={}, aliasing=3, offset=0\n", {1, 2});
}

void test_dead_store_possible_reads() {
  const std::string first = "  Store %0, 1:Int32, flags={}, aliasing=3, offset=0\n";
  const std::string last = "  Store %0, 2:Int32, flags={}, aliasing=3, offset=0\n";
  check_dead_stores(first + "  %2 = Load %0, type=Int8, flags={}, aliasing=3, offset=3\n" + last, {1, 2});
  check_dead_stores(first + "  %2 = Load %1, type=Int32, flags={}, aliasing=3, offset=0\n" + last, {1, 2});
  check_dead_stores(first + "  %2 = Load %1, type=Int32, flags={}, aliasing=0, offset=0\n" + last, {1, 2});
  check_dead_stores(first + "  Call @external:Ptr, type=Void, call_conv=Default, flags={}, aliasing=3\n" + last, {1, 2});
  check_dead_stores(first + "  Jump block=b1\nb1:\n" + last, {1, 2});
}

void test_dead_store_negative_exact_aliasing() {
  const std::string first = "  Store %0, 1:Int32, flags={}, aliasing=-1, offset=0\n";
  const std::string last = "  Store %1, 2:Int32, flags={}, aliasing=-1, offset=0\n";
  check_dead_stores(first + last, {2});
  check_dead_stores(first + "  %2 = Load %0, type=Int32, flags={}, aliasing=-2, offset=4\n" + last, {2});
  check_dead_stores(first + "  %2 = Load %1, type=Int32, flags={}, aliasing=-1, offset=0\n" + last, {1, 2});
  check_dead_stores(first + "  Store %0, 3:Int32, flags={}, aliasing=-2, offset=4\n" +
                    "  %2 = Load %0, type=Int32, flags={}, aliasing=-2, offset=4\n" +
                    last + "  Store %0, 4:Int32, flags={}, aliasing=-2, offset=4\n", {3, 2, 4});
  check_dead_stores(first + "  %2 = Load %1, type=Int32, flags={}, aliasing=0, offset=0\n" + last, {1, 2});
  check_dead_stores(first + "  Call @external:Ptr, type=Void, call_conv=Default, flags={}, aliasing=0\n" + last, {1, 2});
  check_dead_stores(first + "  Jump block=b1\nb1:\n" + last, {1, 2});
}

int main(int argc, char** argv) {
  unittest::Suite suite(argc, argv);
  suite.test("dead store exact overwrite").run(test_dead_store_exact_overwrite);
  suite.test("dead store possible reads").run(test_dead_store_possible_reads);
  suite.test("dead store negative exact aliasing").run(test_dead_store_negative_exact_aliasing);
  return suite.finish();
}
