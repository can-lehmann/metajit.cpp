#include "../jitir.hpp"
#include "../x86gen.hpp"
#include "../../unittest.cpp/unittest.hpp"

using namespace metajit;

int main(int argc, char** argv) {
  unittest::Suite suite(argc, argv);

  suite.test("jump_threading").run([]() {
    enum class Shape { Chain, Work, SelfLoop, Cycle, Invert, Fallthrough };
    for (auto mode : {X86CodeGen::Mode::JIT, X86CodeGen::Mode::AOT}) {
      for (auto shape : {Shape::Chain, Shape::Work, Shape::SelfLoop, Shape::Cycle, Shape::Invert, Shape::Fallthrough}) {
        Context context;
        Allocator allocator;
        Section section(context, allocator);
        Builder builder(&section);
        Block* entry = builder.build_block({Type::Bool, Type::Ptr});
        Block* exit = builder.build_block();
        Block* first = builder.build_block();
        Block* second = builder.build_block();
        Block* last = builder.build_block();

        builder.move_to_end(entry);
        if (shape == Shape::Invert) {
          builder.build_branch(entry->arg(0), first, last);
        } else if (shape == Shape::Fallthrough) {
          builder.build_jump(first);
        } else {
          builder.build_branch(entry->arg(0), first, exit);
        }
        builder.move_to_end(exit);
        builder.build_exit();
        builder.move_to_end(first);
        if (shape == Shape::Work) {
          builder.build_store(entry->arg(1), builder.build_const(Type::Int64, 42), AliasingGroup(0), 0);
        }
        builder.build_jump(second);
        builder.move_to_end(second);
        if (shape == Shape::SelfLoop) {
          builder.build_jump(second);
        } else if (shape == Shape::Cycle) {
          builder.build_jump(first);
        } else if (shape == Shape::Invert || shape == Shape::Fallthrough) {
          builder.build_jump(exit);
        } else {
          builder.build_jump(last);
        }
        builder.move_to_end(last);
        builder.build_exit();
        section.autoname();
        section.set_ordering(BlockOrdering::Natural);

        X86CodeGen codegen(&section, {Reg::X86_R12(), Reg::X86_R13()}, mode);
        std::ostringstream stream;
        codegen.write(stream);
        std::string assembly = stream.str();
        std::string entry_assembly = assembly.substr(0, assembly.find("\nb1") + 1);
        if (shape == Shape::Chain) {
          unittest_assert(entry_assembly.find("jne imm=b4\n") != std::string::npos);
        } else if (shape == Shape::Work) {
          unittest_assert(entry_assembly.find("jne imm=b2\n") != std::string::npos);
        } else if (shape == Shape::SelfLoop) {
          unittest_assert(entry_assembly.find("jne imm=b3\n") != std::string::npos);
          unittest_assert(assembly.find("jmp imm=b3\n") != std::string::npos);
        } else if (shape == Shape::Cycle) {
          unittest_assert(entry_assembly.find("jne imm=b2\n") != std::string::npos ||
                          entry_assembly.find("jne imm=b3\n") != std::string::npos);
        } else if (shape == Shape::Invert) {
          unittest_assert(entry_assembly.find("je imm=b4\n") != std::string::npos);
          unittest_assert(entry_assembly.find("jmp ") == std::string::npos);
        } else {
          unittest_assert(entry_assembly.find("jmp ") == std::string::npos);
        }
      }
    }
  });

  return suite.finish();
}
