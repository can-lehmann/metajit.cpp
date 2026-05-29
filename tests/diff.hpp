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
#include "llvm/IR/Verifier.h"
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
    Section* copy_and_fold(Section* section) {
      // recreate the section using the TraceBuilder
      std::stringstream ss;
      section->write(ss);
      std::string section_str = ss.str();
      std::istringstream iss(section_str);
      return SectionReader<TraceBuilder>::read_section(section->context(), section->allocator(), iss);
    }

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
    protected:
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

      void keep(Value* value) {
        // Keep the value alive by storing it to the data buffer, but don't check equivalence
        size_t offset = alloc(value->type());
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
                                           bool verify_interpreter = true,
                                           bool verify_aot = true) {
      
      
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

      if (llvm::verifyModule(*module, &llvm::errs())) {
        throw std::runtime_error("Generated LLVM IR module verification failed");
      }

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

      X86Func aot_func = nullptr;
      if (verify_aot) {
        X86CodeGen aot_x86cg(section, { Reg::phys(12) }, X86CodeGen::Mode::AOT);
        
        if (!output_path.empty()) {
          std::ofstream stream(output_path + "_aot_x86.asm");
          aot_x86cg.write(stream);
          aot_x86cg.save(output_path + "_aot_x86.bin");
        }

        aot_func = (X86Func) aot_x86cg.deploy();
      }

      Section* folded_section = nullptr;
      if (optimize_section_for_interpreter) {
        folded_section = copy_and_fold(section);
      }
      uint8_t* llvm_data = new uint8_t[data.data_size()]();
      uint8_t* x86_data = new uint8_t[data.data_size()]();
      uint8_t* aot_data = new uint8_t[data.data_size()]();
      uint8_t* interp_data = new uint8_t[data.data_size()]();
      uint8_t* interp_data2 = new uint8_t[data.data_size()]();

      for (size_t sample = 0; sample < sample_count; sample++) {
        data.gen(llvm_data);
        std::copy(llvm_data, llvm_data + data.data_size(), x86_data);
        std::copy(llvm_data, llvm_data + data.data_size(), aot_data);
        std::copy(llvm_data, llvm_data + data.data_size(), interp_data);
        std::copy(llvm_data, llvm_data + data.data_size(), interp_data2);

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

        if (verify_aot) {
          aot_func(aot_data);
        }

        if (optimize_section_for_interpreter) {
          Interpreter interpreter(folded_section, {
            Interpreter::Bits::constant(interp_data2)
          });

          Interpreter::Event event = interpreter.run();
          if (event != Interpreter::Event::Exit) {
            throw unittest::AssertionError(
              "Interpreter of folded section did not exit cleanly",
              __LINE__,
              __FILE__
            );
          }
        }
        
        for (const TestData::Output& output : data.outputs()) {
          bool is_equal = true;
          for (size_t it = 0; it < type_size(output.type); it++) {
            if (llvm_data[output.offset + it] != x86_data[output.offset + it] ||
                (verify_interpreter && llvm_data[output.offset + it] != interp_data[output.offset + it]) ||
                (verify_aot && llvm_data[output.offset + it] != aot_data[output.offset + it]) ||
                (optimize_section_for_interpreter && llvm_data[output.offset + it] != interp_data2[output.offset + it])) {
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
            if (verify_aot) {
              stream << "AOT x86 Output:\n";
              data.write_outputs(stream, aot_data);
            }
            if (optimize_section_for_interpreter) {
              stream << "Folded Interpreter Output:\n";
              data.write_outputs(stream, interp_data2);
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

    class TraceTestData : public TestData {
    private:
      std::vector<size_t> _static_input_offsets;
      std::vector<RandomRange> _static_input_ranges;
      std::vector<Value*> _frozen_values;
    public:
      TraceTestData() {}
      TraceTestData(Builder& builder): TestData(builder) {}

      const std::vector<size_t>& static_input_offsets() const { return _static_input_offsets; }
      const std::vector<RandomRange>& static_input_ranges() const { return _static_input_ranges; }

      Value* static_input(RandomRange random_range) {
        size_t offset = alloc_input(random_range);
        _static_input_offsets.push_back(offset);
        _static_input_ranges.push_back(random_range);

        Value* input_val = _builder->build_load(
          _data,
          random_range.type(),
          LoadFlags::None,
          AliasingGroup(0),
          offset
        );

        // Update the inputs map with the actual value
        _inputs[offset].value = input_val;

        Value* frozen = _builder->build_promote(input_val);
        _frozen_values.push_back(frozen);
        return frozen;
      }

      void gen_static(uint8_t* data) {
        // Generate random values for static inputs only
        for (size_t idx = 0; idx < _static_input_offsets.size(); idx++) {
          size_t offset = _static_input_offsets[idx];
          uint64_t value = _static_input_ranges[idx].gen();
          for (size_t it = 0; it < type_size(_static_input_ranges[idx].type()); it++) {
            data[offset + it] = (uint8_t)((value >> (it * 8)) & 0xFF);
          }
        }
      }
    };

    class DiffTest: public unittest::BaseTest<DiffTest> {
    private:
      std::string _output_path;
      bool _verify_interpreter = true;
      bool _verify_aot = true;
    public:
      DiffTest(const std::string& name, const std::string& output_path):
        unittest::BaseTest<DiffTest>(name), _output_path(output_path) {}

      DiffTest&& interpreter(bool verify_interpreter) && {
        _verify_interpreter = verify_interpreter;
        return std::move(*this);
      }

      DiffTest&& aot(bool verify_aot) && {
        _verify_aot = verify_aot;
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

          if (!builder.block()->terminator()) {
            builder.build_exit();
          }

          unittest_assert(!section->verify(std::cout));

          check_codegen_differential(
            _output_path + "/" + name(),
            section,
            data,
            1024,
            false,
            _verify_interpreter,
            _verify_aot
          );

          delete section;
        });
      }
    };

    class DiffTestSuite: public unittest::Suite {
    private:
      std::string _output_path;
    public:
      DiffTestSuite(const std::string& output_path, int argc = 0, char** argv = nullptr):
        unittest::Suite(argc, argv), _output_path(output_path) {}

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
      SourceTestSuite(const std::string& output_path, int argc = 0, char** argv = nullptr):
        unittest::Suite(argc, argv), _output_path(output_path) {}

      SourceTest source_test(const std::string& ll_file) {
        std::string name = std::filesystem::path(ll_file).stem().string();
        return SourceTest(name, _output_path, ll_file).suite(*this);
      }
    };

    inline void check_opt_differential(Section* before,
                                       Section* after,
                                       TestData& data,
                                       size_t sample_count = 1024) {
      
      before->autoname();
      after->autoname();

      uint8_t* before_data = new uint8_t[data.data_size()]();
      uint8_t* after_data = new uint8_t[data.data_size()]();

      for (size_t sample = 0; sample < sample_count; sample++) {
        data.gen(before_data);
        std::copy(before_data, before_data + data.data_size(), after_data);

        Interpreter before_interp(before, {
          Interpreter::Bits::constant(before_data)
        });
        Interpreter after_interp(after, {
          Interpreter::Bits::constant(after_data)
        });

        Interpreter::Event before_event = before_interp.run();
        Interpreter::Event after_event = after_interp.run();

        if (before_event != Interpreter::Event::Exit) {
          throw unittest::AssertionError(
            "Before interpreter did not exit cleanly",
            __LINE__,
            __FILE__
          );
        }
        if (after_event != Interpreter::Event::Exit) {
          throw unittest::AssertionError(
            "After interpreter did not exit cleanly",
            __LINE__,
            __FILE__
          );
        }

        for (const TestData::Output& output : data.outputs()) {
          bool is_equal = true;
          for (size_t it = 0; it < type_size(output.type); it++) {
            if (before_data[output.offset + it] != after_data[output.offset + it]) {
              is_equal = false;
              break;
            }
          }
          if (!is_equal) {
            std::ostringstream stream;
            stream << "Inputs:\n";
            data.write_inputs(stream, before_data);
            stream << "Before Optimization Output:\n";
            data.write_outputs(stream, before_data);
            stream << "After Optimization Output:\n";
            data.write_outputs(stream, after_data);
            stream << "\n";
            throw unittest::AssertionError(
              "Output mismatch after optimization",
              __LINE__,
              __FILE__,
              stream.str()
            );
          }
        }
      }
    }

    class OptTest: public unittest::BaseTest<OptTest> {
    private:
      std::string _output_path;
    public:
      OptTest(const std::string& name, const std::string& output_path):
        unittest::BaseTest<OptTest>(name), _output_path(output_path) {}

      void run(const std::function<void(Builder&, TestData&)>& body,
               const std::function<void(Section*)>& optimize) && {
        unittest::BaseTest<OptTest>::run([&]() {
          Context context;
          Allocator allocator;
          Section* section = new Section(context, allocator);

          Builder builder(section);
          builder.move_to_end(builder.build_block({Type::Ptr}));

          TestData data(builder);
          body(builder, data);

          if (!builder.block()->terminator()) {
            builder.build_exit();
          }

          {
            std::ofstream stream(_output_path + "/" + name() + ".before.jitir");
            section->write(stream);
          }

          unittest_assert(!section->verify(std::cout));

          Section* cloned_section = new Section(context, allocator);
          Clone::run(section, cloned_section);

          optimize(section);

          {
            std::ofstream stream(_output_path + "/" + name() + ".after.jitir");
            section->write(stream);
          }

          unittest_assert(!section->verify(std::cout));

          check_opt_differential(cloned_section, section, data);

          delete section;
        });
      }
    };

    class OptTestSuite: public unittest::Suite {
    private:
      std::string _output_path;
    public:
      OptTestSuite(const std::string& output_path, int argc = 0, char** argv = nullptr):
        unittest::Suite(argc, argv), _output_path(output_path) {}

      OptTest opt_test(const std::string& name) {
        return OptTest(name, _output_path).suite(*this);
      }
    };

    inline void check_trace_differential(std::string output_path,
                                         Section* section,
                                         TraceTestData& data,
                                         size_t static_sample_count = 16,
                                         size_t dynamic_sample_count = 64) {
      section->autoname();

      if (!output_path.empty()) {
        std::ofstream stream(output_path + ".original.jitir");
        section->write(stream);
      }

      // Generate the generating extension
      llvm::LLVMContext llvm_context;
      std::unique_ptr<llvm::Module> genext_module = std::make_unique<llvm::Module>("genext_module", llvm_context);
      LLVMCodeGen::run(section, genext_module.get(), "genext_func", true);

      if (!output_path.empty()) {
        std::error_code error_code;
        llvm::raw_fd_ostream stream(output_path + "_genext_unopt.ll", error_code, llvm::sys::fs::OF_None);
        genext_module->print(stream, nullptr);
      }

      // Optimize the generating extension at O3 to trigger potential bugs
      LLVMCodeGen::optimize_llvm(*genext_module, llvm::OptimizationLevel::O3);

      if (!output_path.empty()) {
        std::error_code error_code;
        llvm::raw_fd_ostream stream(output_path + "_genext.ll", error_code, llvm::sys::fs::OF_None);
        genext_module->print(stream, nullptr);
      }

      llvm::ExitOnError ExitOnErr;
      std::unique_ptr<llvm::orc::LLJIT> jit = ExitOnErr(llvm::orc::LLJITBuilder().create());
      ExitOnErr(metajit::map_symbols(*jit));

      if (llvm::verifyModule(*genext_module, &llvm::errs())) {
        throw std::runtime_error("Generated LLVM IR module verification failed");
      }

      ExitOnErr(jit->addIRModule(llvm::orc::ThreadSafeModule(
        std::move(genext_module),
        std::make_unique<llvm::LLVMContext>()
      )));

      using GenExtFunc = void(*)(uint8_t*, void*);
      GenExtFunc genext_func = ExitOnErr(jit->lookup("genext_func")).toPtr<GenExtFunc>();

      uint8_t* static_data = new uint8_t[data.data_size()]();

      for (size_t static_sample = 0; static_sample < static_sample_count; static_sample++) {
        // Pick random values for static inputs
        data.gen_static(static_data);

        // Execute generating extension to produce trace
        Context trace_context;
        Allocator trace_allocator;
        Section* trace_section = new Section(trace_context, trace_allocator);
        TraceBuilder trace_builder(trace_section);
        std::vector<Type> args = {Type::Ptr};
        trace_builder.move_to_end(trace_builder.build_block(args));

        genext_func(static_data, &trace_builder);

        trace_builder.build_exit();

        if (!output_path.empty()) {
          std::ofstream stream(output_path + ".trace" + std::to_string(static_sample) + ".jitir");
          trace_section->write(stream);
        }

        if (trace_section->verify(std::cout)) {
          std::ostringstream stream;
          stream << "Trace verification failed for static sample " << static_sample << "\n";
          trace_section->write(stream);
          throw unittest::AssertionError(
            "Generated trace verification failed",
            __LINE__,
            __FILE__,
            stream.str()
          );
        }

        // Now test the trace with random dynamic inputs
        uint8_t* original_data = new uint8_t[data.data_size()]();
        uint8_t* trace_data = new uint8_t[data.data_size()]();

        for (size_t dynamic_sample = 0; dynamic_sample < dynamic_sample_count; dynamic_sample++) {
          // Generate random values for all inputs (including static ones)
          data.gen(original_data);
          // Copy static input values to original_data
          for (size_t idx = 0; idx < data.static_input_offsets().size(); idx++) {
            size_t offset = data.static_input_offsets()[idx];
            Type type = data.static_input_ranges()[idx].type();
            for (size_t it = 0; it < type_size(type); it++) {
              original_data[offset + it] = static_data[offset + it];
            }
          }
          std::copy(original_data, original_data + data.data_size(), trace_data);

          // Run original section
          Interpreter original_interp(section, {
            Interpreter::Bits::constant(original_data)
          });
          Interpreter::Event original_event = original_interp.run();

          // Run traced section
          Interpreter trace_interp(trace_section, {
            Interpreter::Bits::constant(trace_data)
          });
          Interpreter::Event trace_event = trace_interp.run();

          if (original_event != Interpreter::Event::Exit) {
            throw unittest::AssertionError(
              "Original interpreter did not exit cleanly",
              __LINE__,
              __FILE__
            );
          }

          if (trace_event != Interpreter::Event::Exit) {
            throw unittest::AssertionError(
              "Trace interpreter did not exit cleanly",
              __LINE__,
              __FILE__
            );
          }

          // Compare outputs
          for (const TestData::Output& output : data.outputs()) {
            bool is_equal = true;
            for (size_t it = 0; it < type_size(output.type); it++) {
              if (original_data[output.offset + it] != trace_data[output.offset + it]) {
                is_equal = false;
                break;
              }
            }
            if (!is_equal) {
              std::ostringstream stream;
              stream << "Static sample: " << static_sample << ", Dynamic sample: " << dynamic_sample << "\n";
              stream << "Inputs:\n";
              data.write_inputs(stream, original_data);
              stream << "Original Output:\n";
              data.write_outputs(stream, original_data);
              stream << "Trace Output:\n";
              data.write_outputs(stream, trace_data);
              stream << "\n";
              throw unittest::AssertionError(
                "Output mismatch between original and trace",
                __LINE__,
                __FILE__,
                stream.str()
              );
            }
          }
        }

        delete[] original_data;
        delete[] trace_data;
        delete trace_section;
      }

      delete[] static_data;
    }

    class GenExtTest: public unittest::BaseTest<GenExtTest> {
    private:
      std::string _output_path;
      size_t _static_sample_count = 16;
      size_t _dynamic_sample_count = 64;
    public:
      GenExtTest(const std::string& name, const std::string& output_path):
        unittest::BaseTest<GenExtTest>(name), _output_path(output_path) {}

      GenExtTest&& samples(size_t static_samples, size_t dynamic_samples) && {
        _static_sample_count = static_samples;
        _dynamic_sample_count = dynamic_samples;
        return std::move(*this);
      }

      void run(const std::function<void(Builder&, TraceTestData&)>& body) && {
        unittest::BaseTest<GenExtTest>::run([&]() {
          Context context;
          Allocator allocator;
          Section* section = new Section(context, allocator);

          Builder builder(section);
          builder.move_to_end(builder.build_block({Type::Ptr}));

          TraceTestData data(builder);
          body(builder, data);

          if (!builder.block()->terminator()) {
            builder.build_exit();
          }

          unittest_assert(!section->verify(std::cout));

          check_trace_differential(
            _output_path + "/" + name(),
            section,
            data,
            _static_sample_count,
            _dynamic_sample_count
          );

          delete section;
        });
      }
    };

    class GenExtTestSuite: public unittest::Suite {
    private:
      std::string _output_path;
    public:
      GenExtTestSuite(const std::string& output_path, int argc = 0, char** argv = nullptr):
        unittest::Suite(argc, argv), _output_path(output_path) {}

      GenExtTest gen_ext_test(const std::string& name) {
        return GenExtTest(name, _output_path).suite(*this);
      }
    };

    class InterpreterTest: public unittest::BaseTest<InterpreterTest> {
    public:
      struct TestCase {
        std::vector<Interpreter::Bits> entry_args;
        std::map<Value*, Interpreter::Bits> expected_values;
        
        TestCase() {}
        TestCase(const std::vector<Interpreter::Bits>& _entry_args,
                 const std::map<Value*, Interpreter::Bits>& _expected_values):
          entry_args(_entry_args), expected_values(_expected_values) {}
      };
    private:
      std::string _output_path;
    public:
      InterpreterTest(const std::string& name, const std::string& output_path):
        unittest::BaseTest<InterpreterTest>(name), _output_path(output_path) {}
      
      void run(const std::function<std::vector<TestCase>(Builder&)> build) {
        unittest::BaseTest<InterpreterTest>::run([&]() {
          Context context;
          Allocator allocator;
          Section* section = new Section(context, allocator);

          Builder builder(section);
          std::vector<TestCase> test_cases = build(builder);

          if (!builder.block()->terminator()) {
            builder.build_exit();
          }

          unittest_assert(!section->verify(std::cout));

          for (const TestCase& test_case : test_cases) {
            Interpreter interpreter(section, test_case.entry_args);
            Interpreter::Event event = interpreter.run();
            if (event != Interpreter::Event::Exit) {
              throw unittest::AssertionError(
                "Interpreter did not exit cleanly",
                __LINE__,
                __FILE__
              );
            }

            for (const auto& [value, expected] : test_case.expected_values) {
              Interpreter::Bits actual = interpreter.at(value);
              if (actual != expected) {
                std::ostringstream stream;
                stream << "Expected value ";
                expected.write(stream);
                stream << ", but got ";
                actual.write(stream);
                stream << " for ";
                value->write_arg(stream);
                stream << "\n";
                throw unittest::AssertionError(
                  "Interpreter produced unexpected value",
                  __LINE__,
                  __FILE__,
                  stream.str()
                );
              }
            }
          }
        });
      }
    };

    class InterpreterTestSuite: public unittest::Suite {
    private:
      std::string _output_path;
    public:
      InterpreterTestSuite(const std::string& output_path, int argc = 0, char** argv = nullptr):
        unittest::Suite(argc, argv), _output_path(output_path) {}
      
      InterpreterTest interpreter_test(const std::string& name) {
        return InterpreterTest(name, _output_path).suite(*this);
      }
    };
  }
}
