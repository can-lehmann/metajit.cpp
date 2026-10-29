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

from . import ir, matcher
from .lower import Function, Lowerer, Parameter, Program, call, member_call, operation_name
from .signatures import SIGNATURES


class SimplifyLowerer(Lowerer):
    # generate code suitable for Simplify/SimplifyTrace
    def __init__(self, name):
        super().__init__(name, ())
        self.parameters = (Parameter(ir.Value("inst"), "inst"),
                           Parameter(ir.Value("facts"), "facts"))
        self.root = ir.Ref(self.parameters[0].value)
        self.facts = ir.Unary("&", ir.Ref(self.parameters[1].value))
        self.result_type = member_call(self.root, "type")
        self.paths = {}

    def candidate(self, candidate, block, paths, constants):
        replacement = matcher.normalize(candidate.checked.rule.replacement, candidate.checked,
                                        candidate.checked.type_representatives)
        if matcher.equivalent(candidate.pattern, replacement):
            return
        super().candidate(candidate, block, paths, constants)

    def function(self, opcode_matcher):
        block = ir.Block()
        for index in range(len(SIGNATURES[self.name].parameters)):
            self.paths[(index,)] = self.bind(block, "operand", member_call(self.root, "arg", ir.Literal(index)))
        for stage in opcode_matcher.stages:
            self.decision(stage, block, self.paths, {})
        block.statements.append(ir.Return(ir.Literal(None)))
        ir.validate(block, tuple(parameter.value for parameter in self.parameters))
        return Function("try_simplify_" + operation_name(self.name), self.parameters, block)


def lower(program):
    functions = [SimplifyLowerer(name).function(opcode_matcher)
                 for name, opcode_matcher in program.matchers.items()]
    parameter = Parameter(ir.Value("inst"), "inst")
    facts = Parameter(ir.Value("facts"), "facts")
    root = ir.Ref(parameter.value)
    block = ir.Block()
    if not program.matchers:
        block.statements.append(ir.ExprStmt(call("static_cast<void>", root)))
    for name in program.matchers:
        matched = ir.Value("matched")
        block.statements.append(ir.Bind(matched, call(f"dynamic_cast<{name}Inst*>", root)))
        body = ir.Block([ir.Return(call("try_simplify_" + operation_name(name),
                                       ir.Ref(matched), ir.Ref(facts.value)))])
        block.statements.append(ir.If(ir.Binary("!=", ir.Ref(matched), ir.Literal(None)), body))
    block.statements.append(ir.Return(ir.Literal(None)))
    functions.append(Function("try_simplify", (parameter, facts), block))
    return Program(program.rule_names, tuple(functions))
