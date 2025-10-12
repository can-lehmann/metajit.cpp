# Copyright 2025 Can Joshua Lehmann
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from lwir import *

class LLVMAPIPlugin:
    def __init__(self, prefix, type_substitutions):
        self.prefix = prefix
        self.type_substitutions = type_substitutions

    def run(self, ir):
        defs = ""
        for inst in ir.insts:
            defs += f"  llvm::FunctionCallee {inst.format_builder_name(ir)};\n"
        
        inits = ""
        for inst in ir.insts:
            arg_types = ["llvm::PointerType::get(context, 0)"] # Builder
            for arg in inst.args:
                arg_types.append(self.type_substitutions[arg.type])
            arg_types = ", ".join(arg_types)
            inits += f"    {inst.format_builder_name(ir)} = module->getOrInsertFunction(\n"
            inits += f"      \"{self.prefix}_{inst.format_builder_name(ir)}\",\n"
            inits += f"      llvm::FunctionType::get(\n"
            inits += f"        llvm::PointerType::get(context, 0),\n"
            inits += f"        std::vector<llvm::Type*>({{ {arg_types} }}),\n"
            inits += f"        false\n"
            inits += f"      )\n"
            inits += f"    );\n"
        
        return {
            "llvmapi_defs": defs,
            "llvmapi_inits": inits
        }

class BuildBuildInstPlugin:
    def __init__(self, type_substitutions):
        self.type_substitutions = type_substitutions

    def run(self, ir):
        code = f"inline llvm::Value* build_build_inst(llvm::IRBuilder<>& builder,\n"
        code += f"                                    LLVM_API& api,\n"
        code += f"                                    Inst* inst,\n"
        code += f"                                    llvm::Value* jitir_builder,\n"
        code += f"                                    std::vector<llvm::Value*> args) {{\n"
        code += f"  llvm::LLVMContext& context = builder.GetInsertBlock()->getModule()->getContext();\n"
        for inst in ir.insts:
            name = inst.format_name(ir)
            code += f"  if ({name}* i = dynamic_cast<{name}*>(inst)) {{\n"
            code += f"    args.insert(args.begin(), jitir_builder);\n"
            for arg in inst.args:
                if not arg.type.is_value():
                    code += f"    args.push_back(llvm::ConstantInt::get({self.type_substitutions[arg.type]}, (uint64_t)i->{arg.name}(), false));\n"
            code += f"    assert(args.size() == {len(inst.args) + 1});\n"
            code += f"    return builder.CreateCall(api.{inst.format_builder_name(ir)}, args);\n"
            code += f"  }}\n"
        code += f"  assert(false && \"Unknown instruction\");\n"
        code += f"  return nullptr;\n"
        code += f"}}\n"
        return {"build_build_inst": code}

class MapSymbolsInstPlugin:
    def __init__(self, prefix):
        self.prefix = prefix

    def run(self, ir):
        code = ""
        for inst in ir.insts:
            name = f"{self.prefix}_{inst.format_builder_name(ir)}"
            code += f"map_symbol({name});\n"
        return {"map_symbols": code}

class AllocatorBuilderPlugin(BuilderPlugin):
    def __init__(self):
        super().__init__(allocator = self.allocator)
    
    def allocator(self, inst, ir):
        name = inst.format_name(ir)
        arg_count = 0
        for arg in inst.args:
            if arg.type.is_value():
                arg_count += 1
        size = f"sizeof({name}) + sizeof(Value*) * {arg_count}"
        return f"({name}*) _section->allocator().alloc({size}, alignof({name}))"

class InstTrailingConstructorPlugin:
    def run(self, inst, ir):
        name = inst.format_name(ir)
        base = inst.base or ir.inst_base

        init_list = []
        arg_init = ""
        arg_count = 0
        for arg in inst.args:
            match arg.type:
                case ValueType():
                    arg_init += f".with({arg_count}, {arg.name})"
                    arg_count += 1
                case Type():
                    init_list.append(f"_{arg.name}({arg.name})")
                case _:
                    assert False, f"Unknown type: {arg.type}"
        
        arg_init = f"Span<Value*>::trailing(this, {arg_count})" + arg_init

        init_list = [f"{base}({inst.type}, {arg_init})"] + init_list
        init_list = ", ".join(init_list)
        
        ctor_args = ", ".join(inst.format_formal_args(ir))
        code = f"  {name}({ctor_args}): {init_list} {{\n"
        for check in inst.type_checks:
            code += f"    assert({check});\n"
        code += f"  }}\n"
        code += "\n"

        return code

def binop(name, type_checks = None):
    if type_checks is None:
        type_checks = ["is_int(a->type())"]
    return Inst(name,
        args = [Arg("a"), Arg("b")],
        type = "a->type()",
        type_checks = [
            "a->type() == b->type()",
            *type_checks
        ]
    )

def cmp(name, type_checks):
    return Inst(name,
        args = [Arg("a"), Arg("b")],
        type = "Type::Bool",
        type_checks = [
            "a->type() == b->type()",
            *type_checks
        ]
    )

jitir = IR(
    insts = [
        Inst("Const",
            args = [Arg("type", Type("Type")), Arg("value", Type("uint64_t"))],
            type = "type",
            type_checks = []
        ),
        Inst("Freeze",
            args = [Arg("a")],
            type = "a->type()",
            type_checks = []
        ),
        Inst("Input",
            args = [
                Arg("id", Type("size_t")),
                Arg("type", Type("Type")),
                Arg("flags", Type("InputFlags"))
            ],
            type = "type",
            type_checks = []
        ),
        Inst("Output",
            args = [Arg("value"), Arg("id", Type("size_t"))],
            type = "Type::Void",
            type_checks = []
        ),
        Inst("Select",
            args = [Arg("cond", getter=Getter.Always), Arg("a"), Arg("b")],
            type = "a->type()",
            type_checks = [
                "cond->type() == Type::Bool",
                "a->type() == b->type()"
            ]
        ),
        Inst("ResizeU",
            args = [Arg("a"), Arg("type", Type("Type"))],
            type = "type",
            type_checks = [
                "is_int_or_bool(a->type())",
                "is_int_or_bool(type)"
            ]
        ),
        Inst("Load",
            args = [
                Arg("ptr", getter=Getter.Always),
                Arg("type", Type("Type")),
                Arg("flags", Type("LoadFlags")),
                Arg("aliasing", Type("AliasingGroup")),
                Arg("offset", Type("uint64_t"))
            ],
            type = "type",
            type_checks = ["ptr->type() == Type::Ptr"]
        ),
        Inst("Store",
            args = [
                Arg("ptr", getter=Getter.Always),
                Arg("value", getter=Getter.Always),
                Arg("aliasing", Type("AliasingGroup")),
                Arg("offset", Type("uint64_t"))
            ],
            type = "Type::Void",
            type_checks = ["ptr->type() == Type::Ptr"]
        ),
        Inst("AddPtr",
            args = [
                Arg("ptr", getter=Getter.Always),
                Arg("offset", getter=Getter.Always)
            ],
            type = "Type::Ptr",
            type_checks = [
                "ptr->type() == Type::Ptr",
                "offset->type() == Type::Int64"
            ]
        ),
        Inst("AddPtrConst",
            args = [
                Arg("ptr", getter=Getter.Always),
                Arg("offset", Type("uint64_t"))
            ],
            type = "Type::Ptr",
            type_checks = [
                "ptr->type() == Type::Ptr"
            ]
        ),
        binop("Add"),
        binop("Sub"),
        binop("Mul"),
        binop("ModS"),
        binop("ModU"),
        binop("And", type_checks = ["is_int_or_bool(a->type())"]),
        binop("Or", type_checks = ["is_int_or_bool(a->type())"]),
        binop("Xor", type_checks = ["is_int_or_bool(a->type())"]),
        binop("ShrU"),
        binop("ShrS"),
        binop("Shl"),
        cmp("Eq", type_checks = []),
        cmp("LtU", type_checks = ["is_int(a->type())"]),
        cmp("LtS", type_checks = ["is_int(a->type())"]),
        Inst("Branch",
            args = [
                Arg("cond", getter=Getter.Always),
                Arg("true_block", type=Type("Block*")),
                Arg("false_block", type=Type("Block*"))
            ],
            type = "Type::Void",
            type_checks = ["cond->type() == Type::Bool"]
        ),
        Inst("Jump",
            args = [Arg("block", type=Type("Block*"))],
            type = "Type::Void",
            type_checks = []
        ),
        Inst("Exit",
            args = [],
            type = "Type::Void",
            type_checks = []
        )
    ]
)

lwir(
    template_path = "jitir.tmpl.hpp",
    output_path = "jitir.hpp",
    ir = jitir,
    plugins = [
        InstPlugin([
            InstTrailingConstructorPlugin(),
            InstGetterPlugin(),
            InstWritePlugin(custom={
                Type("Block*"): lambda value, stream: f"{value}->write_arg({stream});",
            })
        ]),
        AllocatorBuilderPlugin(),
        CAPIPlugin(
            prefix = "jitir",
            builder_name = "::metajit::TraceBuilder",
            type_substitutions = {
                Type("size_t"): "uint64_t",
                Type("uint64_t"): "uint64_t",
                Type("Type"): "uint32_t",
                Type("LoadFlags"): "uint32_t",
                Type("InputFlags"): "uint32_t",
                Type("Block*"): "void*",
                Type("AliasingGroup"): "uint32_t", # Needs to be passed by LLVM IR
                ValueType(): "void*"
            }
        )
    ]
)

llvm_type_substitutions = {
    Type("size_t"): "llvm::Type::getInt64Ty(context)",
    Type("uint64_t"): "llvm::Type::getInt64Ty(context)",
    Type("Type"): "llvm::Type::getInt32Ty(context)",
    Type("LoadFlags"): "llvm::Type::getInt32Ty(context)",
    Type("InputFlags"): "llvm::Type::getInt32Ty(context)",
    Type("Block*"): "llvm::PointerType::get(context, 0)",
    Type("AliasingGroup"): "llvm::Type::getInt32Ty(context)",
    ValueType(): "llvm::PointerType::get(context, 0)",
}

lwir(
    template_path = "jitir_llvmapi.tmpl.hpp",
    output_path = "jitir_llvmapi.hpp",
    ir = jitir,
    plugins = [
        LLVMAPIPlugin(
            prefix = "jitir",
            type_substitutions = llvm_type_substitutions
        ),
        BuildBuildInstPlugin(
            type_substitutions = llvm_type_substitutions
        ),
        MapSymbolsInstPlugin(
            prefix = "jitir"
        )
    ]
)
