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

#pragma once

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/TargetSelect.h"

#include "../../unittest.cpp/unittest.hpp"

#include "../jitir.hpp"
#include "../llvmgen.hpp"
#include "../x86gen.hpp"

namespace metajit {
  namespace test {
    class TestData {
    private:
      Builder& _builder;
      Input* _data;
      size_t _data_size = 0;
      std::vector<size_t> _input_bytes;
      std::vector<size_t> _output_bytes;

      size_t alloc(Type type) {
        if (_data_size % type_size(type) != 0) {
          _data_size += type_size(type) - (_data_size % type_size(type));
        }
        size_t offset = _data_size;
        _data_size += type_size(type);
        return offset;
      }
    public:
      TestData(Builder& builder): _builder(builder) {
        _data = _builder.build_input(Type::Ptr);
      }

      size_t data_size() const { return _data_size; }
      const std::vector<size_t>& input_bytes() const { return _input_bytes; }
      const std::vector<size_t>& output_bytes() const { return _output_bytes; }

      Value* input(Type type) {
        size_t offset = alloc(type);
        for (size_t it = 0; it < type_size(type); it++) {
          _input_bytes.push_back(offset + it);
        }
        return _builder.build_load(
          _data,
          type,
          LoadFlags::None,
          AliasingGroup(0),
          offset
        );
      }

      void output(Value* value) {
        size_t offset = alloc(value->type());
        for (size_t it = 0; it < type_size(value->type()); it++) {
          _output_bytes.push_back(offset + it);
        }
        _builder.build_store(
          _data,
          value,
          AliasingGroup(0),
          offset
        );
      }
    };

    inline void check_codegen_differential(Section* section, TestData& data, size_t sample_count = 100) {
      llvm::LLVMContext llvm_context;
      std::unique_ptr<llvm::Module> module = std::make_unique<llvm::Module>("module", llvm_context);
      LLVMCodeGen::run(section, module.get(), "llvm_func");

      llvm::ExitOnError ExitOnErr;

      std::unique_ptr<llvm::orc::LLJIT> jit = ExitOnErr(llvm::orc::LLJITBuilder().create());
      ExitOnErr(metajit::map_symbols(*jit));

      ExitOnErr(jit->addIRModule(llvm::orc::ThreadSafeModule(
        std::move(module),
        std::make_unique<llvm::LLVMContext>()
      )));

      using LLVMFunc = void(*)(uint8_t*);
      using X86Func = void(* [[clang::preserve_none]])(uint8_t*);
      LLVMFunc llvm_func = ExitOnErr(jit->lookup("llvm_func")).toPtr<LLVMFunc>();

      X86CodeGen x86cg(section, { Reg::phys(12) });
      X86Func x86_func = (X86Func) x86cg.deploy();

      uint8_t* llvm_data = new uint8_t[data.data_size()]();
      uint8_t* x86_data = new uint8_t[data.data_size()]();

      for (size_t sample = 0; sample < sample_count; sample++) {
        for (size_t offset : data.input_bytes()) {
          llvm_data[offset] = (uint8_t) (rand() % 256);
          x86_data[offset] = llvm_data[offset];
        }

        llvm_func(llvm_data);
        x86_func(x86_data);

        for (size_t offset : data.output_bytes()) {
          unittest_assert(llvm_data[offset] == x86_data[offset]);
        }
      }
    }

    class DiffTest: public unittest::Test {
    private:
    public:
      DiffTest(const std::string& name): unittest::Test(name) {}

      void run(const std::function<void(Builder&, TestData&)>& body) && {
        unittest::Test::run([&]() {
          Context context;
          Allocator allocator;
          Section* section = new Section(context, allocator);

          Builder builder(section);
          builder.move_to_end(builder.build_block());

          TestData data(builder);
          body(builder, data);

          builder.build_exit();

          unittest_assert(!section->verify(std::cout));

          check_codegen_differential(section, data);

          delete section;
        });
      }
    };
  }
}
