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

def binop(name):
    return Inst(name,
        args = [Arg("a"), Arg("b")],
        type = "a->type()",
        type_checks = [
            "a->type() == b->type()",
            "is_int(a->type())"
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

lwir(
    template_path = "jitir.tmpl.hpp",
    output_path = "jitir.hpp",
    ir = IR(
        insts = [
            Inst("Const",
                args = [Arg("value", Type("uint64_t"))],
                type = "Type::Int64",
                type_checks = []
            ),
            Inst("Input",
                args = [Arg("id", Type("size_t")), Arg("type", Type("Type"))],
                type = "type",
                type_checks = []
            ),
            Inst("Output",
                args = [Arg("value"), Arg("id", Type("size_t"))],
                type = "Type::Void",
                type_checks = []
            ),
            Inst("Select",
                args = [Arg("cond"), Arg("a"), Arg("b")],
                type = "a->type()",
                type_checks = [
                    "cond->type() == Type::Bool",
                    "a->type() == b->type()"
                ]
            ),
            Inst("Load",
                args = [Arg("ptr"), Arg("type", Type("Type"))],
                type = "type",
                type_checks = ["ptr->type() == Type::Ptr"]
            ),
            Inst("Store",
                args = [Arg("ptr"), Arg("value")],
                type = "Type::Void",
                type_checks = ["ptr->type() == Type::Ptr"]
            ),
            Inst("AddPtr",
                args = [Arg("ptr"), Arg("offset")],
                type = "Type::Ptr",
                type_checks = [
                    "ptr->type() == Type::Ptr",
                    "is_int(offset->type())"
                ]
            ),
            binop("Add"),
            binop("Sub"),
            binop("Mul"),
            binop("And"),
            binop("Or"),
            binop("Xor"),
            binop("ShrU"),
            binop("ShrS"),
            binop("Shl"),
            cmp("Eq", type_checks = []),
            cmp("LtU", type_checks = ["is_int(a->type())"]),
            cmp("LtS", type_checks = ["is_int(a->type())"])
        ]
    ),
    plugins = [
        InstPlugin([
            InstGetterPlugin(),
            InstWritePlugin()
        ]),
        BuilderPlugin()
    ]
)
