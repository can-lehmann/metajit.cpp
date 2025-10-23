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
#define binop_x86_inst(name, lowercase, usedef, is_64_bit) x86_inst(name, lowercase, usedef, is_64_bit)
#endif

#ifndef rev_binop_x86_inst
#define rev_binop_x86_inst(name, lowercase, usedef, is_64_bit) x86_inst(name, lowercase, usedef, is_64_bit)
#endif

#ifndef imm_binop_x86_inst
#define imm_binop_x86_inst(name, lowercase, usedef, is_64_bit) x86_inst(name, lowercase, usedef, is_64_bit)
#endif

#ifndef x86_inst
#define x86_inst(name, lowercase, usedef, is_64_bit)
#endif

#define mov_usedef { use(rm); def(reg); }
#define mov_mem_usedef { use(reg); def(rm); }
#define binop_usedef { use(reg); use(rm); def(reg); }
#define imm_usedef { use(rm); def(rm); }
#define cmp_usedef { use(reg); use(rm); }
#define cmp_imm_usedef { use(rm); }

binop_x86_inst(Mov8, mov8, mov_usedef, false)
binop_x86_inst(Mov16, mov16, mov_usedef, false)
binop_x86_inst(Mov32, mov32, mov_usedef, false)
binop_x86_inst(Mov64, mov64, mov_usedef, true)
rev_binop_x86_inst(Mov8Mem, mov8_mem, mov_mem_usedef, false)
rev_binop_x86_inst(Mov16Mem, mov16_mem, mov_mem_usedef, false)
rev_binop_x86_inst(Mov32Mem, mov32_mem, mov_mem_usedef, false)
rev_binop_x86_inst(Mov64Mem, mov64_mem, mov_mem_usedef, true)
x86_inst(Mov64Imm, mov64_imm, { def(reg); }, true)
x86_inst(Lea64, lea64, { use(rm); def(reg); }, true)

binop_x86_inst(Add64, add64, binop_usedef, true)
binop_x86_inst(Sub64, sub64, binop_usedef, true)
binop_x86_inst(IMul64, imul64, binop_usedef, true)

imm_binop_x86_inst(Add64Imm, add64_imm, imm_usedef, true)
imm_binop_x86_inst(Sub64Imm, sub64_imm, imm_usedef, true)

binop_x86_inst(And64, and64, binop_usedef, true)
binop_x86_inst(Or64, or64, binop_usedef, true)
binop_x86_inst(Xor64, xor64, binop_usedef, true)

imm_binop_x86_inst(And64Imm, and64_imm, imm_usedef, true)
imm_binop_x86_inst(Or64Imm, or64_imm, imm_usedef, true)
imm_binop_x86_inst(Xor64Imm, xor64_imm, imm_usedef, true)

binop_x86_inst(Shr64, shr64, binop_usedef, true)
binop_x86_inst(Sar64, sar64, binop_usedef, true)

binop_x86_inst(Cmp8, cmp8, cmp_usedef, false)
binop_x86_inst(Cmp16, cmp16, cmp_usedef, false)
binop_x86_inst(Cmp32, cmp32, cmp_usedef, false)
binop_x86_inst(Cmp64, cmp64, cmp_usedef, true)

imm_binop_x86_inst(Cmp8Imm, cmp8_imm, cmp_imm_usedef, false)
imm_binop_x86_inst(Cmp16Imm, cmp16_imm, cmp_imm_usedef, false)
imm_binop_x86_inst(Cmp32Imm, cmp32_imm, cmp_imm_usedef, false)
imm_binop_x86_inst(Cmp64Imm, cmp64_imm, cmp_imm_usedef, true)

binop_x86_inst(Test64, test64, cmp_usedef, true)

x86_inst(SetE8, sete8, { def(rm); }, false)
x86_inst(SetL8, setl8, { def(rm); }, false)
x86_inst(SetB8, setb8, { def(rm); }, false)

binop_x86_inst(CMovZ64, cmovz64, binop_usedef, true)
binop_x86_inst(CMovE64, cmove64, binop_usedef, true)
binop_x86_inst(CMovL64, cmovl64, binop_usedef, true)
binop_x86_inst(CMovB64, cmovb64, binop_usedef, true)

x86_inst(Ret, ret, {}, true)

#undef x86_inst
#undef binop_x86_inst
#undef rev_binop_x86_inst
#undef imm_binop_x86_inst
