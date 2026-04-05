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
#include <filesystem>

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"

#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/TargetSelect.h"

#include "../../unittest.cpp/unittest.hpp"

#include "../jitir.hpp"
#include "../llvmgen.hpp"
#include "../x86gen.hpp"
#include "../lowerllvm.hpp"

namespace metajit {
  namespace test {
    class RandomRange {
    private:
      Type _type = Type::Void;
      uint64_t _min = 0;
      uint64_t _max = 0;
    public:
      RandomRange() {}
      RandomRange(Type type):
        _type(type), _min(0), _max(type_mask(type)) {}
      RandomRange(Type type, uint64_t min, uint64_t max):
        _type(type), _min(min), _max(max) {}
      
      Type type() const { return _type; }
      uint64_t min() const { return _min; }
      uint64_t max() const { return _max; }

      bool is_full() const {
        return _min == 0 && _max == type_mask(_type);
      }

      uint64_t gen() const {
        assert(_min <= _max);
        uint64_t value = 0;
        for (size_t it = 0; it < sizeof(value); it++) {
          value |= ((uint64_t)(rand() & 0xff)) << (it * 8);
        }
        if (_max != ~(uint64_t) 0 || _min != 0) {
          assert(_max - _min + 1 != 0);
          value = _min + (value % (_max - _min + 1));
        }
        return value;
      }

      Value* gen_const(Builder& builder) const {
        return builder.build_const(_type, gen());
      }

      void write(std::ostream& stream) const {
        stream << _type << " in [" << _min << "; " << _max << "]";
      }
    };

    class TestData {
    public:
      struct RandomInput {
        Value* value = nullptr;
        RandomRange range;

        RandomInput() {}
        RandomInput(Value* _value, const RandomRange& _range):
          value(_value), range(_range) {}
      };

      struct Output {
        size_t offset = 0;
        Type type = Type::Void;
        Value* value = nullptr;
      };
    private:
      Builder* _builder = nullptr;
      Arg* _data = nullptr;
      size_t _data_size = 0;
      std::vector<size_t> _input_bytes;
      std::map<size_t, RandomInput> _inputs;
      std::vector<Output> _outputs;

      size_t alloc(Type type) {
        if (_data_size % type_size(type) != 0) {
          _data_size += type_size(type) - (_data_size % type_size(type));
        }
        size_t offset = _data_size;
        _data_size += type_size(type);
        return offset;
      }
    public:
      TestData() {}
      TestData(Builder& builder): _builder(&builder) {
        _data = _builder->entry_arg(0);
      }

      size_t data_size() const { return _data_size; }
      const std::vector<size_t>& input_bytes() const { return _input_bytes; }
      const std::vector<Output>& outputs() const { return _outputs; }

      size_t alloc_input(RandomRange random_range) {
        size_t offset = alloc(random_range.type());
        for (size_t it = 0; it < type_size(random_range.type()); it++) {
          _input_bytes.push_back(offset + it);
        }
        _inputs[offset] = RandomInput(nullptr, random_range);
        return offset;
      }

      size_t alloc_output(Type type) {
        size_t offset = alloc(type);
        
        Output output;
        output.offset = offset;
        output.type = type;
        _outputs.push_back(output);

        return offset;
      }

      Value* input(RandomRange random_range) {
        assert(_builder && _data);
        size_t offset = alloc_input(random_range);
        Value* value = _builder->build_load(
          _data,
          random_range.type(),
          LoadFlags::None,
          AliasingGroup(0),
          offset
        );
        _inputs[offset].value = value;
        return value;
      }

      void output(Value* value) {
        assert(_builder && _data);
        size_t offset = alloc_output(value->type());
        _outputs.back().value = value;
        _builder->build_store(
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

    public:
      void write_inputs(std::ostream& stream, uint8_t* data) {
        for (auto [offset, input] : _inputs) {
          stream << "  ";
          input.value->write_arg(stream);
          stream << " = ";
          write_value_of_type(stream, input.value->type(), data + offset);
          stream << "\n";
        }
      }

      void write_outputs(std::ostream& stream, uint8_t* data) {
        for (const Output& output : _outputs) {
          stream << "  ";
          if (output.value) {
            output.value->write_arg(stream);
          } else {
            stream << "[" << output.offset << "]";
          }
          stream << " = ";
          write_value_of_type(stream, output.type, data + output.offset);
          stream << "\n";
        }
      }

      void gen(uint8_t* data) {
        for (auto [offset, input] : _inputs) {
          uint64_t value = input.range.gen();
          // Assume little-endian, which is fine since we are on x86_64
          for (size_t it = 0; it < type_size(input.range.type()); it++) {
            data[offset + it] = (uint8_t)((value >> (it * 8)) & 0xFF);
          }
        }
      }
    };

    inline void check_codegen_differential(std::string output_path,
                                           Section* section,
                                           TestData& data,
                                           size_t sample_count = 1024,
                                           bool optimize_section_for_interpreter = false,
                                           bool verify_interpreter = true) {
      
      
      section->autoname();
      
      if (!output_path.empty()) {
        std::ofstream stream(output_path + ".jitir");
        section->write(stream);
      }

      if (!output_path.empty()) {
        std::ofstream stream(output_path + ".domtree.dot");
        DominatorTree dom_tree(section);
        dom_tree.write_dot(stream);
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
      uint8_t* interp_data = new uint8_t[data.data_size()]();

      for (size_t sample = 0; sample < sample_count; sample++) {
        data.gen(llvm_data);
        std::copy(llvm_data, llvm_data + data.data_size(), x86_data);
        std::copy(llvm_data, llvm_data + data.data_size(), interp_data);

        llvm_func(llvm_data);
        x86_func(x86_data);

        if (verify_interpreter) {
          Interpreter interpreter(section, {
            Interpreter::Bits::constant(interp_data)
          });

          Interpreter::Event event = interpreter.run();
          if (event != Interpreter::Event::Exit) {
            throw unittest::AssertionError(
              "Interpreter did not exit cleanly",
              __LINE__,
              __FILE__
            );
          }
        }
        
        for (const TestData::Output& output : data.outputs()) {
          bool is_equal = true;
          for (size_t it = 0; it < type_size(output.type); it++) {
            if (llvm_data[output.offset + it] != x86_data[output.offset + it] ||
                (verify_interpreter && llvm_data[output.offset + it] != interp_data[output.offset + it])) {
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
            if (verify_interpreter) {
              stream << "Interpreter Output:\n";
              data.write_outputs(stream, interp_data);
            }
            stream << "\n";
            throw unittest::AssertionError(
              "Output mismatch",
              __LINE__,
              __FILE__,
              stream.str()
            );
          }
        }
        if (verify_interpreter && optimize_section_for_interpreter && sample == sample_count / 2) {
          // optimize the section. that way the second half of the samples
          // runs in the interpreter with the optimized section,
          // spotting bugs in the optimizer
          DeadCodeElim::run(section);
          RefineAliasing::run(section);
          DeadStoreElim::run(section);
          metajit::DeadCodeElim::run(section);
          Simplify::run(section, 10);
          DeadCodeElim::run(section);
        }
      }
    }

    class DiffTest: public unittest::BaseTest<DiffTest> {
    private:
      std::string _output_path;
      bool _verify_interpreter = true;
    public:
      DiffTest(const std::string& name, const std::string& output_path):
        unittest::BaseTest<DiffTest>(name), _output_path(output_path) {}

      DiffTest&& interpreter(bool verify_interpreter) && {
        _verify_interpreter = verify_interpreter;
        return std::move(*this);
      }

      void run(const std::function<void(Builder&, TestData&)>& body) && {
        unittest::BaseTest<DiffTest>::run([&]() {
          Context context;
          Allocator allocator;
          Section* section = new Section(context, allocator);

          Builder builder(section);
          builder.move_to_end(builder.build_block({Type::Ptr}));

          TestData data(builder);
          body(builder, data);

          builder.build_exit();

          unittest_assert(!section->verify(std::cout));

          check_codegen_differential(
            _output_path + "/" + name(),
            section,
            data,
            1024,
            false,
            _verify_interpreter
          );

          delete section;
        });
      }
    };

    class DiffTestSuite: public unittest::Suite {
    private:
      std::string _output_path;
    public:
      DiffTestSuite(const std::string& output_path):
        unittest::Suite(), _output_path(output_path) {}

      DiffTest diff_test(const std::string& name) {
        return DiffTest(name, _output_path).suite(*this);
      }
    };

    class SourceTest: public unittest::BaseTest<SourceTest> {
    private:
      std::string _output_path;
      std::string _ll_file;

      std::vector<RandomRange> _inputs;
      std::vector<Type> _outputs;
    public:
      SourceTest(const std::string& name,
                 const std::string& output_path,
                 const std::string& ll_file):
        unittest::BaseTest<SourceTest>(name),
        _output_path(output_path),
        _ll_file(ll_file) {}

      void run() && {
        unittest::BaseTest<SourceTest>::run([&]() {
          llvm::LLVMContext llvm_context;
          llvm::SMDiagnostic error;
          std::unique_ptr<llvm::Module> module = llvm::parseIRFile(_ll_file, error, llvm_context);
          
          if (!module) {
            std::string error_msg;
            llvm::raw_string_ostream stream(error_msg);
            error.print("test_source", stream);
            throw unittest::AssertionError(
              "Unable to parse LLVM IR",
              __LINE__,
              __FILE__,
              error_msg
            );
          }

          llvm::Function* function = module->getFunction("run");
          if (!function) {
            throw unittest::AssertionError(
              "Expected function `run` in LLVM IR",
              __LINE__,
              __FILE__,
              _ll_file
            );
          }

          Context context;
          Allocator allocator;
          Section* section = new Section(context, allocator);

          LowerLLVM(function, section);
          unittest_assert(!section->verify(std::cout));

          TestData data;
          for (const RandomRange& range : _inputs) {
            data.alloc_input(range);
          }
          for (const Type& type : _outputs) {
            data.alloc_output(type);
          }

          check_codegen_differential(
            _output_path + "/" + name(),
            section,
            data,
            128
          );

          delete section;
        });
      }

      SourceTest&& inputs(const std::vector<RandomRange>& input_ranges) && {
        _inputs = input_ranges;
        return std::move(*this);
      }

      SourceTest&& outputs(const std::vector<Type>& output_types) && {
        _outputs = output_types;
        return std::move(*this);
      }
    };

    class SourceTestSuite: public unittest::Suite {
    private:
      std::string _output_path;
    public:
      SourceTestSuite(const std::string& output_path):
        unittest::Suite(), _output_path(output_path) {}

      SourceTest source_test(const std::string& ll_file) {
        std::string name = std::filesystem::path(ll_file).stem().string();
        return SourceTest(name, _output_path, ll_file).suite(*this);
      }
    };
  }
}
