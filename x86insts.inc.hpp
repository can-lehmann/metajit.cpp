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
#define binop_x86_inst(name, lowercase, usedef, is_64_bit, opcode) x86_inst(name, lowercase, usedef, is_64_bit, opcode)
#endif

#ifndef rev_binop_x86_inst
#define rev_binop_x86_inst(name, lowercase, usedef, is_64_bit, opcode) x86_inst(name, lowercase, usedef, is_64_bit, opcode)
#endif

#ifndef imm_binop_x86_inst
#define imm_binop_x86_inst(name, lowercase, usedef, is_64_bit, opcode) x86_inst(name, lowercase, usedef, is_64_bit, opcode)
#endif

#ifndef jmp_x86_inst
#define jmp_x86_inst(name, lowercase, usedef, is_64_bit, opcode) x86_inst(name, lowercase, usedef, is_64_bit, opcode)
#endif

#ifndef unop_x86_inst
#define unop_x86_inst(name, lowercase, usedef, is_64_bit, opcode) x86_inst(name, lowercase, usedef, is_64_bit, opcode)
#endif

#ifndef x86_inst
#define x86_inst(name, lowercase, usedef, is_64_bit, opcode)
#endif

#define mov_usedef { use(rm); def(reg); }
#define mov_mem_usedef { use(reg); def(rm); }
#define binop_usedef { use(reg); use(rm); def(reg); }
#define imm_usedef { use(rm); def(rm); }
#define cmp_usedef { use(reg); use(rm); }
#define cmp_imm_usedef { use(rm); }

binop_x86_inst(Mov8, mov8, mov_usedef, false, { rex(); byte(0x8a); modrm(); })
binop_x86_inst(Mov16, mov16, mov_usedef, false, { rex_opt(); byte(0x8b); modrm(); })
binop_x86_inst(Mov32, mov32, mov_usedef, false, { rex_opt(); byte(0x8b); modrm(); })
binop_x86_inst(Mov64, mov64, mov_usedef, true, { rex_w(); byte(0x8b); modrm(); })

rev_binop_x86_inst(Mov8Mem, mov8_mem, mov_mem_usedef, false, { rex(); byte(0x88); modrm(); })
rev_binop_x86_inst(Mov16Mem, mov16_mem, mov_mem_usedef, false, { rex_opt(); byte(0x89); modrm(); })
rev_binop_x86_inst(Mov32Mem, mov32_mem, mov_mem_usedef, false, { rex_opt(); byte(0x89); modrm(); })
rev_binop_x86_inst(Mov64Mem, mov64_mem, mov_mem_usedef, true, { rex_w(); byte(0x89); modrm(); })

imm_binop_x86_inst(Mov8Imm, mov8_imm, { def(reg); }, true, { reg = Reg::phys(0); rex(); byte(0xc6); modrm(); imm_n(1); })
imm_binop_x86_inst(Mov16Imm, mov16_imm, { def(reg); }, true, { reg = Reg::phys(0); rex_opt(); byte(0xc7); modrm(); imm_n(2); })
imm_binop_x86_inst(Mov32Imm, mov32_imm, { def(reg); }, true, { reg = Reg::phys(0); rex_opt(); byte(0xc7); modrm(); imm_n(4); })
imm_binop_x86_inst(Mov64Imm, mov64_imm, { def(reg); }, true, { reg = Reg::phys(0); rex_w(); byte(0xc7); modrm(); imm_n(4); })
x86_inst(Mov64Imm64, mov64_imm64, { def(reg); }, true, { rex_w(); byte(0xb8 + (reg.id() & 0x111)); imm_n(8); })

x86_inst(Lea64, lea64, { use(rm); def(reg); }, true, { rex_w(); byte(0x8d); modrm(); })

binop_x86_inst(Add64, add64, binop_usedef, true, { rex_w(); byte(0x03); modrm(); })
binop_x86_inst(Sub64, sub64, binop_usedef, true, { rex_w(); byte(0x2b); modrm(); })
binop_x86_inst(IMul64, imul64, binop_usedef, true, { rex_w(); byte(0x0f); byte(0xaf); modrm(); })
unop_x86_inst(Div, div, { use(rm); }, true, { reg = Reg::phys(6); rex_w(); byte(0xf7); modrm(); })

imm_binop_x86_inst(Add64Imm, add64_imm, imm_usedef, true, { reg = Reg::phys(0); rex_w(); byte(0x81); modrm(); imm_n(4); })
imm_binop_x86_inst(Sub64Imm, sub64_imm, imm_usedef, true, { reg = Reg::phys(5); rex_w(); byte(0x81); modrm(); imm_n(4); })

binop_x86_inst(And64, and64, binop_usedef, true, { rex_w(); byte(0x23); modrm(); })
binop_x86_inst(Or64, or64, binop_usedef, true, { rex_w(); byte(0x0b); modrm(); })
binop_x86_inst(Xor64, xor64, binop_usedef, true, { rex_w(); byte(0x31); modrm(); })

imm_binop_x86_inst(And64Imm, and64_imm, imm_usedef, true, { reg = Reg::phys(4); rex_w(); byte(0x81); modrm(); imm_n(4); })
imm_binop_x86_inst(Or64Imm, or64_imm, imm_usedef, true, { reg = Reg::phys(1); rex_w(); byte(0x81); modrm(); imm_n(4); })
imm_binop_x86_inst(Xor64Imm, xor64_imm, imm_usedef, true, { reg = Reg::phys(6); rex_w(); byte(0x81); modrm(); imm_n(4); })

binop_x86_inst(Shl64, shl64, binop_usedef, true, { reg = Reg::phys(4); rex_w(); byte(0xd3); modrm(); })
binop_x86_inst(Shr64, shr64, binop_usedef, true, { reg = Reg::phys(5); rex_w(); byte(0xd3); modrm(); })
binop_x86_inst(Sar64, sar64, binop_usedef, true, { reg = Reg::phys(7); rex_w(); byte(0xd3); modrm(); })

imm_binop_x86_inst(Shl64Imm, shl64_imm, imm_usedef, true, { reg = Reg::phys(4); rex_w(); byte(0xc1); modrm(); imm_n(1); })
imm_binop_x86_inst(Shr64Imm, shr64_imm, imm_usedef, true, { reg = Reg::phys(5); rex_w(); byte(0xc1); modrm(); imm_n(1); })
imm_binop_x86_inst(Sar64Imm, sar64_imm, imm_usedef, true, { reg = Reg::phys(7); rex_w(); byte(0xc1); modrm(); imm_n(1); })

binop_x86_inst(Cmp8, cmp8, cmp_usedef, false, { rex(); byte(0x38); modrm(); })
binop_x86_inst(Cmp16, cmp16, cmp_usedef, false, { rex_opt(); byte(0x39); modrm(); })
binop_x86_inst(Cmp32, cmp32, cmp_usedef, false, { rex_opt(); byte(0x39); modrm(); })
binop_x86_inst(Cmp64, cmp64, cmp_usedef, true, { rex_w(); byte(0x39); modrm(); })

imm_binop_x86_inst(Cmp8Imm, cmp8_imm, cmp_imm_usedef, false, { reg = Reg::phys(7); rex(); byte(0x80); modrm(); imm_n(1); })
imm_binop_x86_inst(Cmp16Imm, cmp16_imm, cmp_imm_usedef, false, { reg = Reg::phys(7); rex_opt(); byte(0x81); modrm(); imm_n(2); })
imm_binop_x86_inst(Cmp32Imm, cmp32_imm, cmp_imm_usedef, false, { reg = Reg::phys(7); rex_opt(); byte(0x81); modrm(); imm_n(4); })
imm_binop_x86_inst(Cmp64Imm, cmp64_imm, cmp_imm_usedef, true, { reg = Reg::phys(7); rex_w(); byte(0x81); modrm(); imm_n(4); })

binop_x86_inst(Test64, test64, cmp_usedef, true, { rex_w(); byte(0x85); modrm(); })
imm_binop_x86_inst(Test8Imm, test8_imm, cmp_imm_usedef, false, { reg = Reg::phys(0); rex(); byte(0xf6); modrm(); imm_n(1); })

unop_x86_inst(SetE8, sete8, { def(rm); }, false, { rex(); byte(0x0f); byte(0x94); modrm(); })
unop_x86_inst(SetL8, setl8, { def(rm); }, false, { rex(); byte(0x0f); byte(0x9c); modrm(); })
unop_x86_inst(SetB8, setb8, { def(rm); }, false, { rex(); byte(0x0f); byte(0x92); modrm(); })

binop_x86_inst(CMovNZ64, cmovnz64, binop_usedef, true, { rex_w(); byte(0x0f); byte(0x45); modrm(); })
binop_x86_inst(CMovE64, cmove64, binop_usedef, true, { rex_w(); byte(0x0f); byte(0x44); modrm(); })
binop_x86_inst(CMovL64, cmovl64, binop_usedef, true, { rex_w(); byte(0x0f); byte(0x4c); modrm(); })
binop_x86_inst(CMovB64, cmovb64, binop_usedef, true, { rex_w(); byte(0x0f); byte(0x42); modrm(); })

jmp_x86_inst(Jmp, jmp, {}, true, { byte(0xcd); imm_n(4); })
jmp_x86_inst(JNE, jne, {}, true, { byte(0x0f); byte(0x85); imm_n(4); })
jmp_x86_inst(JE, je, {}, true, { byte(0x0f); byte(0x84); imm_n(4); })
jmp_x86_inst(JL, jl, {}, true, { byte(0x0f); byte(0x8c); imm_n(4); })
jmp_x86_inst(JB, jb, {}, true, { byte(0x0f); byte(0x82); imm_n(4); })
x86_inst(Ret, ret, {}, true, { byte(0xc3); })

#undef x86_inst
#undef binop_x86_inst
#undef rev_binop_x86_inst
#undef imm_binop_x86_inst
#undef jmp_x86_inst
#undef unop_x86_inst
