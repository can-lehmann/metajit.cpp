from dataclasses import dataclass

from .source import Span


@dataclass(frozen=True)
class Expr:
    span: Span


@dataclass(frozen=True)
class Name(Expr):
    name: str


@dataclass(frozen=True)
class Number(Expr):
    value: int


@dataclass(frozen=True)
class Call(Expr):
    name: str
    args: tuple[Expr, ...]


@dataclass(frozen=True)
class Typed(Expr):
    value: Expr
    type_name: str


@dataclass(frozen=True)
class Unary(Expr):
    operator: str
    operand: Expr


@dataclass(frozen=True)
class Binary(Expr):
    operator: str
    left: Expr
    right: Expr


@dataclass(frozen=True)
class Compare(Expr):
    operands: tuple[Expr, ...]
    operators: tuple[str, ...]


@dataclass(frozen=True)
class Binding:
    span: Span
    name: str
    value: Expr


@dataclass(frozen=True)
class Condition:
    span: Span
    kind: str
    value: Expr


@dataclass(frozen=True)
class Rule:
    span: Span
    name: str
    pattern: Expr
    body: tuple[Binding | Condition, ...]
    replacement: Expr


def format_pattern(node):
    if isinstance(node, Name):
        return node.name
    if isinstance(node, Number):
        return str(node.value)
    if isinstance(node, Call):
        return f"{node.name}({', '.join(format_pattern(arg) for arg in node.args)})"
    if isinstance(node, Typed):
        return f"{format_pattern(node.value)}: {node.type_name}"
    raise ValueError(f"unsupported pattern {node!r}")


def format_rule(rule):
    return f"{rule.name}: {format_pattern(rule.pattern)} => {format_pattern(rule.replacement)}"
