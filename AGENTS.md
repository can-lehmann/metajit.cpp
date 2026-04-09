# metajit.cpp

metajit.cpp is a meta-tracing framework written in C++.

## JITIR

JITIR is a custom intermediate representation designed for meta-tracing. It is a low-level SSA-based IR similar to LLVM IR.
While many LLVM IR operations map directly to JITIR, JITIR also includes operations that are specifically designed for meta-tracing.
JITIR uses block parameters instead of phi nodes.

Here is an example JITIR program that adds two 32-bit integers:

```
section {
b0(%0: Ptr, %1: Ptr, %2: Ptr):
  %3 = Load %0, type=Int32, flags={}, aliasing=0, offset=0
  %4 = Load %1, type=Int32, flags={}, aliasing=0, offset=0
  %5 = Add %3, %4
  Store %2, %5, aliasing=0, offset=0
  Exit
}
```

metajit.cpp uses a generating extension for tracing.

## Project Structure

- `main.cpp`: A small example program using metajit.cpp.
- `jitir.py`: A Python script that generates JITIR-related headers from templates. It also includes all instruction definitions.
- `interactive.hpp`: A ncurses based interactive debugger which allows a user to single step through JITIR code. The debugger is meant to be used by humans. You should not use it.
- `jitir.tmpl.hpp`: Template for generating the jitir.hpp header file, which defines the JITIR (Intermediate Representation) and its operations.
- `jitir_llvmapi.tmpl.hpp`: Template for generating the jitir_llvmapi.hpp header file, which provides 
- `llvmgen.hpp`: Generates LLVM IR from JITIR and emits the generating extension which is used for tracing.
- `x86gen.hpp`: Our custom x86 backend. LLVM's x86 backend is too slow for our use case, so we generate x86 code directly from JITIR.
- `x86insts.inc.hpp`: x86 instruction definitions and encodings used by x86gen.hpp.
- `lowerllvm.hpp`: Lowers LLVM IR to JITIR.
- `tv.hpp`: Translation validation for JITIR using Z3. This allows us to prove refinement between JITIR programs.
- `doc/`: Documentation, including JITIR instruction documentation generated from templates.
- `tests/`: Unit tests
  - `diff.hpp`: Differential testing. We compare the x86 backend, LLVM backend, and the interpreter against each other to find bugs.
  - `output/`: Output files from the tests. This includes JITIR and assembly output from the backends.
  - `fuzzer.cpp`: A fuzzer that generates random JITIR programs and checks that the backends and interpreter all produce the same output.
  - `test_cfg.cpp`: Tests with control flow
  - `test_insts.cpp`: Tests for individual instructions
  - `test_fuzzer.cpp`: Regression tests for all bugs previously found by the fuzzer.
  - `test_knownbits.cpp`: Tests for knownbits analysis, which is used by the `Simplify` pass.
  - `test_opt.cpp`: Tests for optimization passes.
  - `test_tv.cpp`: Tests for translation validation using Z3.

## Coding Guidelines

- Never edit the generated jitir.hpp and jitir_llvmapi.hpp files directly. Instead, edit the corresponding template files jitir.tmpl.hpp and jitir_llvmapi.tmpl.hpp. The instructions are specified in the jitir.py generator script.
- Never edit any files in tests/output. They are just output files from the unit-tests used to debug failing test cases. They are also not golden tests; in fact they are .gitnored. 
- metajit.cpp is a just in time compiler. This makes compile time a crucial metric to optimize for. Write performant compiler code and avoid allocations where possible.
- Always try to keep changes scoped and reviewable. Attempt to build the minimal change needed to implement a feature or fix a bug, and avoid making unrelated formatting or refactoring changes in the same commit.

## Testing

Make automatically regenerates the JITIR headers.

- You can run the entire test suite using `make test`.
- You can run individual tests using `make tests/test_* && ./tests/test_*`
- You can run the fuzzer using `make fuzz`.
