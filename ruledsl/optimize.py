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

from dataclasses import replace

from . import ir


def optimize_block(block):
    statements = []
    for statement in block.statements:
        if isinstance(statement, ir.If):
            statement = replace(statement, then_block=optimize_block(statement.then_block),
                                else_block=optimize_block(statement.else_block))
        statements.append(statement)
        if isinstance(statement, ir.Return):
            break
    return ir.Block(statements)


def optimize(program):
    return replace(program, functions=tuple(replace(function, body=optimize_block(function.body))
                                            for function in program.functions))
