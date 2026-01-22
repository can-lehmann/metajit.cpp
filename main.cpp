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

#include "jitir.hpp"
#include "llvmgen.hpp"
#include "x86gen.hpp"

int main() {
  using namespace metajit;

  Context context;
  Allocator allocator;
  Section* section = new Section(context, allocator);

  Builder builder(section);
  builder.move_to_end(builder.build_block({
    Type::Ptr, // a
    Type::Ptr, // b
    Type::Ptr  // c
  }));

  Arg* a = builder.entry_arg(0);
  Arg* b = builder.entry_arg(1);
  Arg* c = builder.entry_arg(2);

  builder.build_store(
    c,
    builder.build_add(
      builder.build_load(a, Type::Int32, LoadFlags::None, AliasingGroup(0), 0),
      builder.build_load(b, Type::Int32, LoadFlags::None, AliasingGroup(0), 0)
    ),
    AliasingGroup(0),
    0
  );

  builder.build_exit();

  section->write(std::cout);

  {
    // Save to JSON
    std::ofstream stream("section.json");
    section->write_json(stream);
  }

  {
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> module = std::make_unique<llvm::Module>("my_module", context);

    LLVMCodeGen::run(section, module.get(), "add");

    module->print(llvm::outs(), nullptr);
  }

  {
    // We use the clang::preserve_none calling convention, so the arguments are
    // stored in r12, r13, r14
    X86CodeGen cg(section, { Reg::phys(12), Reg::phys(13), Reg::phys(14) });

    cg.write(std::cout);

    cg.save("asm.bin");

    using Fn = void(* [[clang::preserve_none]])(uint32_t*, uint32_t*, uint32_t*);
    Fn fn = (Fn) cg.deploy();

    uint32_t a = 2;
    uint32_t b = 3;
    uint32_t c = 0;
    fn(&a, &b, &c);
    std::cout << a << " + " << b << " = " << c << std::endl;
  }
  
  
}

