#pragma once

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

namespace metajit {
  class LLVM_API {
  public:
    ${llvmapi_defs}
    llvm::FunctionCallee build_guard;
    llvm::FunctionCallee is_const_inst;

    LLVM_API(llvm::Module* module) {
      llvm::LLVMContext& context = module->getContext();
      ${llvmapi_inits}

      build_guard = module->getOrInsertFunction(
        "jitir_build_guard",
        llvm::FunctionType::get(
          llvm::Type::getVoidTy(context),
          std::vector<llvm::Type*>({
            llvm::PointerType::get(context, 0),
            llvm::PointerType::get(context, 0),
            llvm::Type::getInt32Ty(context)
          }),
          false
        )
      );

      is_const_inst = module->getOrInsertFunction(
        "jitir_is_const_inst",
        llvm::FunctionType::get(
          llvm::Type::getInt32Ty(context),
          std::vector<llvm::Type*>({
            llvm::PointerType::get(context, 0)
          }),
          false
        )
      );
    }
  };

  extern "C" {
    void jitir_build_guard(void* builder_ptr, void* value_ptr, uint32_t expected) {
      Builder& builder = *(Builder*)builder_ptr;
      Value* value = (Value*)value_ptr;

      if (dynmatch(ConstInst, constant, value)) {
        if ((constant->value() && expected) || (!constant->value() && !expected)) {
          return; // Always true
        }
      }

      Block* failure = builder.build_block();
      Block* success = builder.build_block();

      Block* a = success;
      Block* b = failure;
      if (!expected) {
        std::swap(a, b);
      }
      builder.build_branch(value, a, b);
      
      builder.move_to_end(failure);
      builder.build_exit();

      builder.move_to_end(success);
    }

    uint32_t jitir_is_const_inst(void* value_ptr) {
      Value* value = (Value*)value_ptr;
      return dynamic_cast<ConstInst*>(value) != nullptr;
    }
  }

  ${build_build_inst}

  inline llvm::Error map_symbols(llvm::orc::LLJIT& jit) {
    llvm::orc::SymbolMap symbol_map;

    #define map_symbol(name) \
      symbol_map.insert({ \
        jit.mangleAndIntern(#name), \
        llvm::orc::ExecutorSymbolDef( \
          llvm::orc::ExecutorAddr((uint64_t)(void*)(&name)), \
          llvm::JITSymbolFlags::Callable \
        ) \
      });

    ${map_symbols}

    map_symbol(jitir_build_guard)
    map_symbol(jitir_is_const_inst)

    #undef map_symbol

    llvm::orc::JITDylib& dylib = jit.getMainJITDylib();
    return dylib.define(llvm::orc::absoluteSymbols(std::move(symbol_map)));
  }
}
