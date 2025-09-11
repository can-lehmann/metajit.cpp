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

#include <variant>
#include <fstream>

#include "jitir.hpp"

namespace metajit {
  struct X86Inst {
    enum class Kind {
      Mov32, MovImm32, Add, IMul, Ret
    };

    struct Mem {
      Value* base = nullptr;
      size_t scale = 0;
      Value* index = nullptr;
      int32_t disp = 0;

      Mem() {}
      Mem(Value* _base): base(_base) {}
      Mem(Value* _base, int32_t _disp): base(_base), disp(_disp) {}
    };

    using ModRM = std::variant<Value*, Mem>;

    Kind kind;
    Value* a = nullptr;
    ModRM b = nullptr;
    uint32_t imm = 0;

    X86Inst() = default;
    X86Inst(Kind _kind): kind(_kind) {}
    X86Inst(Kind _kind, Value* _a, ModRM _b):
      kind(_kind), a(_a), b(_b) {}
    X86Inst(Kind _kind, ModRM _b, uint32_t _imm):
      kind(_kind), b(_b), imm(_imm) {}
    
    bool has_imm() const {
      return kind == Kind::MovImm32;
    }

    bool has_modrm() const {
      return kind == Kind::Mov32 || kind == Kind::MovImm32 || kind == Kind::Add;
    }
  };

  class X86CodeGen {
  private:
    Section* _section;
    ValueMap<std::vector<X86Inst>> _isel;
    ValueMap<size_t> _regs;
    ValueMap<bool> _used;

    void isel() {
      for (Block* block : *_section) {
        for (Inst* inst : *block) {
          if (dynmatch(ConstInst, constant, inst)) {
            _isel[inst] = {X86Inst(X86Inst::Kind::MovImm32, inst, constant->value())};
          } else if (dynmatch(InputInst, input, inst)) {
            _isel[inst] = {};
          } else if (dynmatch(OutputInst, input, inst)) {
            _isel[inst] = {};
          } else if (dynmatch(LoadInst, load, inst)) {
            X86Inst::Kind opcode;
            switch (load->type()) {
              case Type::Int32: opcode = X86Inst::Kind::Mov32; break;
              default: assert(false && "Unsupported load type");
            }
            _isel[inst] = {
              X86Inst(opcode, inst, X86Inst::Mem(load->arg(0)))
            };
          } else if (dynmatch(AddInst, add, inst)) {
            _isel[inst] = {
              X86Inst(X86Inst::Kind::Mov32, inst, add->arg(0)),
              X86Inst(X86Inst::Kind::Add, inst, add->arg(1))
            };
          } else if (dynmatch(MulInst, mul, inst)) {
            _isel[inst] = {
              X86Inst(X86Inst::Kind::Mov32, inst, mul->arg(0)),
              X86Inst(X86Inst::Kind::IMul, inst, mul->arg(1))
            };
          } else if (dynmatch(ExitInst, exit, inst)) {
            _isel[inst] = {
              X86Inst(X86Inst::Kind::Ret)
            };
          } else {
            inst->write(std::cerr);
            std::cerr << std::endl;
            assert(false && "Unknown instruction");
          }
        }
      }
    }

    void regalloc() {
      for (Block* block : *_section) {
        for (Inst* inst : *block) {
          _regs[inst] = inst->name(); // TODO
        }
      }
    }
  public:
    X86CodeGen(Section* section): _section(section) {
      section->autoname();
      _isel.init(section);
      _regs.init(section);
      _used.init(section);

      isel();
      regalloc();
    }

    void emit(std::vector<uint8_t>& buffer) {
      for (Block* block : *_section) {
        for (Inst* inst : *block) {
          for (const X86Inst& x86inst : _isel[inst]) {
            switch (x86inst.kind) {
              case X86Inst::Kind::Mov32: buffer.push_back(0x8b); break; // mov r32, r/m32
              case X86Inst::Kind::MovImm32: buffer.push_back(0xc7); break; // mov r/m32, imm32
              case X86Inst::Kind::Add: buffer.push_back(0x03); break; // add r32, r/m32
              case X86Inst::Kind::IMul: buffer.push_back(0x0f); buffer.push_back(0xaf); break; // imul r32, r/m32
              case X86Inst::Kind::Ret: buffer.push_back(0xc3); break; // ret
              default: assert(false && "Unknown x86 instruction");
            }
             
            if (x86inst.has_modrm()) {
              // Encode ModRM byte
              uint8_t mod = 0;
              uint8_t reg = _regs[x86inst.a] & 0b111;
              uint8_t rm = 0;
              if (std::holds_alternative<Value*>(x86inst.b)) {
                Value* val = std::get<Value*>(x86inst.b);
                mod = 0b11;
                rm = _regs.at(val) & 0b111;


                uint8_t modrm = mod << 6 | reg << 3 | rm;
                buffer.push_back(modrm);
              } else if (std::holds_alternative<X86Inst::Mem>(x86inst.b)) {
                X86Inst::Mem mem = std::get<X86Inst::Mem>(x86inst.b);

                if (mem.disp == 0) {
                  mod = 0b00;
                } else if (mem.disp >= -128 && mem.disp <= 127) {
                  mod = 0b01;
                } else {
                  mod = 0b10;
                }

                if ((mem.index == nullptr || mem.scale == 0) && (_regs.at(mem.base) & 0b111) != 0b100) {
                  rm = _regs.at(mem.base) & 0b111;

                  uint8_t modrm = mod << 6 | reg << 3 | rm;
                  buffer.push_back(modrm);
                } else {
                  rm = 0b100; // SIB

                  uint8_t modrm = mod << 6 | reg << 3 | rm;
                  buffer.push_back(modrm);
                
                  uint8_t scale = 0;
                  uint8_t index = 0b100; // none
                  uint8_t base = _regs.at(mem.base) & 0b111; // none

                  switch (mem.scale) {
                    case 0: break;
                    case 1: scale = 0b00; break;
                    case 2: scale = 0b01; break;
                    case 4: scale = 0b10; break;
                    case 8: scale = 0b11; break;
                    default: assert(false && "Invalid scale");
                  }

                  if (mem.scale != 0 && mem.index != nullptr) {
                    index = _regs.at(mem.index) & 0b111;
                  }

                  uint8_t sib = scale << 6 | index << 3 | base;
                  buffer.push_back(sib);
                }

                if (mem.disp != 0) {
                  if (mem.disp >= -128 && mem.disp <= 127) {
                    buffer.push_back(mem.disp & 0xff);
                  } else {
                    buffer.push_back(mem.disp & 0xff);
                    buffer.push_back((mem.disp >> 8) & 0xff);
                    buffer.push_back((mem.disp >> 16) & 0xff);
                    buffer.push_back((mem.disp >> 24) & 0xff);
                  }
                }
              }
            }

            if (x86inst.has_imm()) {
              // Encode immediate value (little-endian)
              buffer.push_back(x86inst.imm & 0xff);
              buffer.push_back((x86inst.imm >> 8) & 0xff);
              buffer.push_back((x86inst.imm >> 16) & 0xff);
              buffer.push_back((x86inst.imm >> 24) & 0xff);
            }
          }
        }
      }
    }

    void save(const std::string& filename) {
      std::vector<uint8_t> buffer;
      emit(buffer);

      std::ofstream file(filename, std::ios::binary);
      if (!file) {
        assert(false && "Failed to open file for writing");
      }
      file.write((const char*) buffer.data(), buffer.size());
    }
  };
}
