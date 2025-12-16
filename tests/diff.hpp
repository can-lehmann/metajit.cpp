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

#include <memory>
#include <fstream>

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
      std::map<size_t, Value*> _inputs;
      std::map<size_t, Value*> _outputs;

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
      const std::map<size_t, Value*>& inputs() const { return _inputs; }
      const std::map<size_t, Value*>& outputs() const { return _outputs; }

      Value* input(Type type) {
        size_t offset = alloc(type);
        for (size_t it = 0; it < type_size(type); it++) {
          _input_bytes.push_back(offset + it);
        }
        Value* value = _builder.build_load(
          _data,
          type,
          LoadFlags::None,
          AliasingGroup(0),
          offset
        );
        _inputs[offset] = value;
        return value;
      }

      void output(Value* value) {
        size_t offset = alloc(value->type());
        _outputs[offset] = value;
        _builder.build_store(
          _data,
          value,
          AliasingGroup(0),
          offset
        );
      }
    
    private:
      void write_value_of_type(std::ostream& stream, Type type, uint8_t* data) {
        switch (type) {
          case Type::Bool:
          case Type::Int8:
            stream << (uint32_t)(*(uint8_t*)data);
          break;
          case Type::Int16: stream << *(uint16_t*)data; break;
          case Type::Int32: stream << *(uint32_t*)data; break;
          case Type::Int64: stream << *(uint64_t*)data; break;
          case Type::Ptr: stream << *(void**) data; break;
          default: assert(false && "Unsupported type");
        }
      }

      void write_values(std::ostream& stream,
                        const std::map<size_t, Value*>& values,
                        uint8_t* data) {
        for (auto [offset, value] : values) {
          stream << "  ";
          value->write_arg(stream);
          stream << " = ";
          write_value_of_type(stream, value->type(), data + offset);
          stream << "\n";
        }
      }
    public:
      void write_inputs(std::ostream& stream, uint8_t* data) {
        write_values(stream, _inputs, data);
      }

      void write_outputs(std::ostream& stream, uint8_t* data) {
        write_values(stream, _outputs, data);
      }
    };

    inline void check_codegen_differential(std::string output_path, Section* section, TestData& data, size_t sample_count = 100) {
      if (!output_path.empty()) {
        std::ofstream stream(output_path + ".jitir");
        section->write(stream);
      }

      llvm::LLVMContext llvm_context;
      std::unique_ptr<llvm::Module> module = std::make_unique<llvm::Module>("module", llvm_context);
      LLVMCodeGen::run(section, module.get(), "llvm_func");

      if (!output_path.empty()) {
        std::error_code error_code;
        llvm::raw_fd_ostream stream(output_path + "_llvm.ll", error_code, llvm::sys::fs::OF_None);
        module->print(stream, nullptr);
      }
      
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
      
      if (!output_path.empty()) {
        std::ofstream stream(output_path + "_x86.asm");
        x86cg.write(stream);
        x86cg.save(output_path + "_x86.bin");
      }
      
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

        for (auto [offset, value] : data.outputs()) {
          bool is_equal = true;
          for (size_t it = 0; it < type_size(value->type()); it++) {
            if (llvm_data[offset + it] != x86_data[offset + it]) {
              is_equal = false;
              break;
            }
          }
          if (!is_equal) {
            std::ostringstream stream;
            stream << "Inputs:\n";
            data.write_inputs(stream, llvm_data);
            stream << "LLVM Output:\n";
            data.write_outputs(stream, llvm_data);
            stream << "x86 Output:\n";
            data.write_outputs(stream, x86_data);
            stream << "\n";
            throw unittest::AssertionError(
              "llvm_data[offset] != x86_data[offset]",
              __LINE__,
              __FILE__,
              stream.str()
            );
          }
        }
      }
    }

    class DiffTest: public unittest::Test {
    private:
      std::string _output_path;
    public:
      DiffTest(const std::string& name, const std::string& output_path):
        unittest::Test(name), _output_path(output_path) {}

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

          check_codegen_differential(_output_path + "/" + name(), section, data);

          delete section;
        });
      }
    };
  }
}
