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

#ifndef binop_x86_inst
#define binop_x86_inst(name, lowercase) x86_inst(name, lowercase)
#endif

#ifndef rev_binop_x86_inst
#define rev_binop_x86_inst(name, lowercase) x86_inst(name, lowercase)
#endif

#ifndef imm_binop_x86_inst
#define imm_binop_x86_inst(name, lowercase) x86_inst(name, lowercase)
#endif

#ifndef x86_inst
#define x86_inst(name, lowercase)
#endif

binop_x86_inst(Mov8, mov8)
binop_x86_inst(Mov16, mov16)
binop_x86_inst(Mov32, mov32)
binop_x86_inst(Mov64, mov64)
rev_binop_x86_inst(Mov8Mem, mov8_mem)
rev_binop_x86_inst(Mov16Mem, mov16_mem)
rev_binop_x86_inst(Mov32Mem, mov32_mem)
rev_binop_x86_inst(Mov64Mem, mov64_mem)
x86_inst(Mov64Imm, mov64_imm)

x86_inst(Lea64, lea64)

binop_x86_inst(Add64, add64)
binop_x86_inst(Sub64, sub64)
binop_x86_inst(IMul64, imul64)

imm_binop_x86_inst(Add64Imm, add64_imm)
imm_binop_x86_inst(Sub64Imm, sub64_imm)

binop_x86_inst(And64, and64)
binop_x86_inst(Or64, or64)
binop_x86_inst(Xor64, xor64)

imm_binop_x86_inst(And64Imm, and64_imm)
imm_binop_x86_inst(Or64Imm, or64_imm)
imm_binop_x86_inst(Xor64Imm, xor64_imm)

binop_x86_inst(Shr64, shr64)
binop_x86_inst(Sar64, sar64)

binop_x86_inst(Cmp8, cmp8)
binop_x86_inst(Cmp16, cmp16)
binop_x86_inst(Cmp32, cmp32)
binop_x86_inst(Cmp64, cmp64)

imm_binop_x86_inst(Cmp8Imm, cmp8_imm)
imm_binop_x86_inst(Cmp16Imm, cmp16_imm)
imm_binop_x86_inst(Cmp32Imm, cmp32_imm)
imm_binop_x86_inst(Cmp64Imm, cmp64_imm)

binop_x86_inst(Test64, test64)

x86_inst(SetE8, sete8)
x86_inst(SetL8, setl8)
x86_inst(SetB8, setb8)

binop_x86_inst(CMovZ64, cmovz64)
binop_x86_inst(CMovE64, cmove64)
binop_x86_inst(CMovL64, cmovl64)
binop_x86_inst(CMovB64, cmovb64)

x86_inst(Ret, ret)

#undef x86_inst
#undef binop_x86_inst
#undef rev_binop_x86_inst
#undef imm_binop_x86_inst
