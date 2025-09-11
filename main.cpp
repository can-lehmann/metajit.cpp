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

  Section* section = new Section();
  Builder builder(section);
  builder.move_to_end(builder.build_block());

  builder.build_output(
    builder.build_add(
      builder.build_load(
        builder.build_input(Type::Ptr),
        Type::Int32
      ),
      builder.build_input(Type::Int32)
    )
  );

  builder.build_exit();

  section->write(std::cout);

  {
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> module = std::make_unique<llvm::Module>("my_module", context);

    LLVMCodeGen cg(section, module.get(), true);

    module->print(llvm::outs(), nullptr);
  }

  {
    X86CodeGen cg(section);
    cg.save("asm.bin");

  }
  
  
}

