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

from dataclasses import dataclass, field, fields

from .types import ConcreteType


class Node:
    def iter_children(self):
        for descriptor in fields(self):
            value = getattr(self, descriptor.name)
            if isinstance(value, Node):
                yield value
            elif isinstance(value, (tuple, list)):
                for item in value:
                    if isinstance(item, Node):
                        yield item

    def walk(self, node_type=None):
        if node_type is None or isinstance(self, node_type):
            yield self
        for child in self.iter_children():
            yield from child.walk(node_type)


@dataclass(frozen=True, eq=False)
class Value:
    name_hint: str


class Expr(Node):
    pass


@dataclass(frozen=True)
class Ref(Expr):
    value: Value


@dataclass(frozen=True)
class Literal(Expr):
    value: int | bool | None | ConcreteType


@dataclass(frozen=True)
class Symbol(Expr):
    name: str


@dataclass(frozen=True)
class Call(Expr):
    function: Expr
    args: tuple[Expr, ...] = ()


@dataclass(frozen=True)
class Member(Expr):
    object: Expr
    name: str


@dataclass(frozen=True)
class Unary(Expr):
    operator: str
    operand: Expr


@dataclass(frozen=True)
class BoolToBits(Expr):
    condition: Expr


@dataclass(frozen=True)
class Binary(Expr):
    operator: str
    left: Expr
    right: Expr


@dataclass(frozen=True)
class Conditional(Expr):
    condition: Expr
    then_expr: Expr
    else_expr: Expr


class Stmt(Node):
    pass


@dataclass
class Block(Node):
    statements: list[Stmt] = field(default_factory=list)


@dataclass(frozen=True)
class Bind(Stmt):
    value: Value
    expr: Expr


@dataclass(frozen=True)
class ExprStmt(Stmt):
    expr: Expr


@dataclass(frozen=True)
class Comment(Stmt):
    text: str


@dataclass(frozen=True)
class If(Stmt):
    condition: Expr
    then_block: Block
    else_block: Block = field(default_factory=Block)


@dataclass(frozen=True)
class Return(Stmt):
    expr: Expr | None = None


class InvalidIR(ValueError):
    pass


def validate_refs(expr, available):
    for ref in expr.walk(Ref):
        if ref.value not in available:
            raise InvalidIR(f"unbound value {ref.value.name_hint!r}")


def validate_block(block, available, defined):
    available = set(available)
    for statement in block.statements:
        if isinstance(statement, Bind):
            validate_refs(statement.expr, available)
            if statement.value in defined:
                raise InvalidIR(f"value {statement.value.name_hint!r} is defined more than once")
            available.add(statement.value)
            defined.add(statement.value)
        elif isinstance(statement, ExprStmt):
            validate_refs(statement.expr, available)
        elif isinstance(statement, Comment):
            pass
        elif isinstance(statement, If):
            validate_refs(statement.condition, available)
            validate_block(statement.then_block, available, defined)
            validate_block(statement.else_block, available, defined)
        elif isinstance(statement, Return):
            if statement.expr is not None:
                validate_refs(statement.expr, available)
        else:
            raise InvalidIR(f"unsupported statement {type(statement).__name__!r}")


def validate(block, parameters=()):
    parameters = tuple(parameters)
    if len(set(parameters)) != len(parameters):
        raise InvalidIR("duplicate parameter identity")
    validate_block(block, set(parameters), set(parameters))
