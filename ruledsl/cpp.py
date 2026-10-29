# Copyright 2026 Can Joshua Lehmann
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

import json
import re

from . import ir
from .types import ConcreteType


KEYWORDS = frozenset("""
alignas alignof and and_eq asm atomic_cancel atomic_commit atomic_noexcept auto
bitand bitor bool break case catch char char8_t char16_t char32_t class compl concept
const consteval constexpr constinit const_cast continue co_await co_return co_yield
decltype default delete do double dynamic_cast else enum explicit export extern false
float for friend goto if inline int long mutable namespace new noexcept not not_eq
nullptr operator or or_eq private protected public reflexpr register reinterpret_cast
requires return short signed sizeof static static_assert static_cast struct switch
synchronized template this thread_local throw true try typedef typeid typename union
unsigned using virtual void volatile wchar_t while xor xor_eq
""".split())
RESERVED = KEYWORDS | {"Type", "Value", "Const", "Facts", "RuledslUnknownBits", "uint64_t",
                       "ruledsl_rule_counts", "ruledsl_rule_names"}


class Emitter:
    def __init__(self):
        self.lines = []
        self.indent = 0
        self.names = {}
        self.used_names = set(RESERVED)

    def line(self, text):
        self.lines.append("  " * self.indent + text)

    def name_for(self, value):
        if value not in self.names:
            hint = re.sub(r"[^A-Za-z0-9_]", "_", value.name_hint).strip("_") or "value"
            if hint[0].isdigit():
                hint = "v_" + hint
            hint = re.sub(r"_+", "_", hint)
            name = hint
            suffix = 1
            while name in self.used_names:
                name = f"{hint}_{suffix}"
                suffix += 1
            self.used_names.add(name)
            self.names[value] = name
        return self.names[value]

    def expression(self, node):
        if isinstance(node, ir.Ref):
            return self.name_for(node.value)
        if isinstance(node, ir.Literal):
            if node.value is None:
                return "nullptr"
            if isinstance(node.value, bool):
                return "true" if node.value else "false"
            if isinstance(node.value, ConcreteType):
                return f"Type::{node.value.name}"
            return f"0x{node.value & ((1 << 64) - 1):x}ULL"
        if isinstance(node, ir.Symbol):
            return node.name
        if isinstance(node, ir.Member):
            return f"({self.expression(node.object)})->{node.name}"
        if isinstance(node, ir.Call):
            arguments = ", ".join(self.expression(argument) for argument in node.args)
            return f"{self.expression(node.function)}({arguments})"
        if isinstance(node, ir.Unary):
            operator = "!" if node.operator == "not" else node.operator
            return f"({operator}{self.expression(node.operand)})"
        if isinstance(node, ir.BoolToBits):
            return f"uint64_t({self.expression(node.condition)})"
        if isinstance(node, ir.Binary):
            operator = {"and": "&&", "or": "||", "//": "/"}.get(node.operator, node.operator)
            return f"({self.expression(node.left)} {operator} {self.expression(node.right)})"
        if isinstance(node, ir.Conditional):
            return (f"({self.expression(node.condition)} ? {self.expression(node.then_expr)}"
                    f" : {self.expression(node.else_expr)})")
        raise ValueError(f"unsupported expression {node!r}")

    def block(self, block):
        for statement in block.statements:
            if isinstance(statement, ir.Bind):
                self.line(f"[[maybe_unused]] auto {self.name_for(statement.value)} = {self.expression(statement.expr)};")
            elif isinstance(statement, ir.Comment):
                self.line("// " + statement.text)
            elif isinstance(statement, ir.If):
                condition = self.expression(statement.condition)
                if isinstance(statement.condition, (ir.Binary, ir.Unary)):
                    condition = condition[1:-1]
                self.line(f"if ({condition}) {{")
                self.indent += 1
                self.block(statement.then_block)
                self.indent -= 1
                if statement.else_block.statements:
                    self.line("} else {")
                    self.indent += 1
                    self.block(statement.else_block)
                    self.indent -= 1
                self.line("}")
            elif isinstance(statement, ir.Return):
                value = " " + self.expression(statement.expr) if statement.expr is not None else ""
                self.line(f"return{value};")
            elif isinstance(statement, ir.ExprStmt):
                expression = statement.expr
                if isinstance(expression, ir.Call) and expression.function == ir.Symbol("ruledsl_record"):
                    self.line("#ifdef METAJIT_RULE_STATS")
                    self.line(f"++ruledsl_rule_counts[{self.expression(expression.args[0])}];")
                    self.line("#endif")
                else:
                    self.line(self.expression(expression) + ";")
            else:
                raise ValueError(f"unsupported statement {statement!r}")

    def function(self, function):
        self.names = {}
        self.used_names = set(RESERVED)
        self.used_names.update(symbol.name for symbol in function.body.walk(ir.Symbol))
        self.used_names.add(function.name)
        ir.validate(function.body, tuple(parameter.value for parameter in function.parameters))
        parameters = []
        if any(parameter.kind == "facts" for parameter in function.parameters):
            self.line("template<class Facts = RuledslUnknownBits>")
        for parameter in function.parameters:
            type_ = {"value": "Value*", "type": "Type", "inst": "Inst*",
                     "facts": "[[maybe_unused]] const Facts&"}[parameter.kind]
            default = " = {}" if parameter.kind == "facts" else ""
            parameters.append(f"{type_} {self.name_for(parameter.value)}{default}")
        self.line(f"Value* {function.name}({', '.join(parameters)}) {{")
        self.indent += 1
        self.block(function.body)
        self.indent -= 1
        self.line("}")


def emit(program, include_statistics=True):
    emitter = Emitter()
    if any(parameter.kind == "facts" for function in program.functions for parameter in function.parameters):
        emitter.line("struct RuledslUnknownBits {")
        emitter.indent += 1
        emitter.line("uint64_t known_zeros(Value*) const { return 0; }")
        emitter.line("uint64_t known_ones(Value*) const { return 0; }")
        emitter.indent -= 1
        emitter.line("};")
    if include_statistics and program.rule_names:
        emitter.line("#ifdef METAJIT_RULE_STATS")
        emitter.line(f"inline static uint64_t ruledsl_rule_counts[{len(program.rule_names)}] = {{}};")
        names = ", ".join(json.dumps(name) for name in program.rule_names)
        emitter.line(f"inline static constexpr const char* ruledsl_rule_names[] = {{{names}}};")
        emitter.line("#endif")
    for function in program.functions:
        emitter.function(function)
    return "\n".join(emitter.lines) + ("\n" if emitter.lines else "")
