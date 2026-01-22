# metajit.cpp

A low-level meta-tracing framework.

![Architecture](doc/architecture.svg)

## JITIR

JITIR is metajit.cpp's intermediate representation.
It is a low-level SSA-based IR similar to LLVM IR.
While many LLVM IR operations map directly to JITIR, JITIR also includes operations that are specifically designed for meta-tracing.
JITIR uses block parameters instead of phi nodes.
All instructions are documented in [doc/jitir.md](doc/jitir.md).

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

metajit.cpp's LLVM backend generates the following LLVM IR from this JITIR program:

```llvm
define void @add(ptr %0, ptr %1, ptr %2) {
entry:
  br label %b0

b0:                                               ; preds = %entry
  %3 = phi ptr [ %0, %entry ]
  %4 = phi ptr [ %1, %entry ]
  %5 = phi ptr [ %2, %entry ]
  %6 = load i32, ptr %3, align 4
  %7 = load i32, ptr %4, align 4
  %8 = add i32 %6, %7
  store i32 %8, ptr %5, align 4
  ret void
}
```

The x86 backend generates the following x86-64 assembly code:

```asm
b0:
  mov32 reg=p0 rm=[p12]
  mov32 reg=p1 rm=[p13]
  lea64 reg=p2 rm=[p0 + p1 * 1]
  mov32_mem reg=p2 rm=[p14]
  ret
b1:
```

### Aliasing

Aliasing information is encoded using aliasing groups.
Memory operations can only alias if they are in the same aliasing group.
Aliasing groups are represented as integers.
Negative integers are used to represent exact aliasing (i.e., two operations with the same negative aliasing group always alias).
Non-negative integers are used to represent potential aliasing (i.e., two operations with the same non-negative aliasing group may alias, but do not have to).

## LLVM Frontend

A subset of LLVM IR can be automatically translated to metajit.cpp's JITIR.
This allows us to use clang as a frontend for metajit.cpp.
See https://github.com/can-lehmann/uxnjit for an example of using the LLVM frontend to automatically generate a JIT from a interpreter written in C.

## Differential Testing

Since we have two backends (LLVM and x86), we can use differential testing to find bugs.
In differential testing, a JITIR program is compiled with both backends and the behaviour of the generated code is compared by running both programs with random inputs.

![](doc/fuzzer.svg)

## License

Copyright 2025 Can Joshua Lehmann

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
