from dataclasses import dataclass
import operator
import re

from . import ast
from .parser import parse
from .signatures import SIGNATURES
from .source import TypeCheckError
from .types import BOOL, ConcreteType, CONCRETE_TYPES, INT64, TypeConflict, TypeConstraints, TypeVar


KNOWN_BITS_FUNCTIONS = frozenset({"known_zeros", "known_ones"})


@dataclass(frozen=True)
class ExprInfo:
    kind: str
    type: TypeVar | None = None
    dependencies: frozenset[str] = frozenset()


@dataclass
class CheckedRule:
    rule: ast.Rule
    bindings: dict[str, ExprInfo]
    type_variables: dict[str, TypeVar]
    expressions: dict[ast.Expr, ExprInfo]
    instantiations: tuple[dict[TypeVar, ConcreteType], ...]
    type_representatives: dict[TypeVar, TypeVar]
    type_domains: dict[TypeVar, tuple[ConcreteType, ...]]

    def type_of(self, expression, instantiation):
        variable = self.expressions[expression].type
        return instantiation[variable] if variable is not None else None

    @property
    def requires_facts(self):
        return any(isinstance(node, ast.Call) and node.name in KNOWN_BITS_FUNCTIONS
                   for node in self.expressions)


BINARY_OPERATORS = {
    "+": operator.add, "-": operator.sub, "*": operator.mul,
    "//": operator.floordiv, "%": operator.mod,
    "<<": operator.lshift, ">>": operator.rshift,
    "&": operator.and_, "|": operator.or_, "^": operator.xor,
}
COMPARISON_OPERATORS = {
    "==": operator.eq, "!=": operator.ne,
    "<": operator.lt, "<=": operator.le, ">": operator.gt, ">=": operator.ge,
}
UNARY_OPERATORS = {"+": operator.pos, "-": operator.neg, "~": operator.invert,
                   "not": operator.not_}


def bit_reverse(value, width):
    result = 0
    for index in range(width):
        result = (result << 1) | ((value >> index) & 1)
    return result


def mul_high_u(left, right, width):
    return (left * right) >> width


CALCULATION_FUNCTIONS = {"bit_reverse": bit_reverse, "mul_high_u": mul_high_u}


class Checker:
    def __init__(self, rule, source, filename):
        self.rule = rule
        self.source = source
        self.filename = filename
        self.constraints = TypeConstraints()
        self.bindings = {}
        self.type_variables = {}
        self.expressions = {}
        self.definitions = {}
        self.anchors = set()

    def error(self, node, message):
        raise TypeCheckError(message, node.span, self.source, self.filename)

    def unify(self, left, right, node):
        try:
            self.constraints.unify(left, right)
        except TypeConflict as error:
            self.error(node, str(error))

    def require(self, info, kind, node):
        if info.kind != kind:
            self.error(node, f"expected {kind}, got {info.kind}")

    def type_reference(self, name, node, introduce=False):
        if name in CONCRETE_TYPES:
            return self.constraints.fresh((CONCRETE_TYPES[name],))
        if name in {"Void", "Ptr"} or re.fullmatch(r"(?:Int|Float)[0-9]+", name):
            self.error(node, f"unsupported concrete type {name!r}")
        if name in self.bindings:
            self.error(node, f"{name!r} is already a value binding")
        if name not in self.type_variables:
            if not introduce:
                self.error(node, f"unknown type variable {name!r}")
            self.type_variables[name] = self.constraints.fresh()
        return self.type_variables[name]

    def available_name(self, name, node):
        if name in self.bindings:
            self.error(node, f"duplicate binding {name!r}")
        if name in self.type_variables or name in CONCRETE_TYPES:
            self.error(node, f"{name!r} is already a type name")

    def remember(self, node, info):
        self.expressions[node] = info
        return info

    def pattern(self, node, introduce=False):
        if isinstance(node, ast.Typed):
            reference = self.type_reference(node.type_name, node, introduce)
            info = self.pattern(node.value, introduce)
            self.unify(info.type, reference, node)
        elif isinstance(node, ast.Name):
            if introduce and node.name not in self.bindings:
                self.available_name(node.name, node)
                constant = node.name.startswith("C")
                self.bindings[node.name] = ExprInfo(
                    "constant" if constant else "value", self.constraints.fresh(),
                    frozenset((node.name,)) if constant else frozenset()
                )
            if node.name not in self.bindings:
                self.error(node, f"undefined value {node.name!r}")
            info = self.bindings[node.name]
            if info.kind == "condition":
                info = ExprInfo("constant", self.constraints.fresh((BOOL,)), info.dependencies)
            if info.kind not in {"value", "constant"}:
                self.error(node, f"cannot use {info.kind} as an IR value")
        elif isinstance(node, ast.Number):
            info = ExprInfo("constant", self.constraints.fresh())
        elif isinstance(node, ast.Call):
            if node.name not in SIGNATURES:
                self.error(node, f"unknown or unsupported instruction {node.name!r}")
            signature = SIGNATURES[node.name]
            if len(node.args) != len(signature.parameters):
                self.error(node, f"{node.name} expects {len(signature.parameters)} operands, "
                           f"got {len(node.args)}")
            variables = {name: self.constraints.fresh(domain)
                         for name, domain in signature.domains}
            for argument, parameter in zip(node.args, signature.parameters):
                argument_info = self.pattern(argument, introduce)
                self.unify(argument_info.type, variables[parameter], argument)
            info = ExprInfo("value", variables[signature.result])
        else:
            self.error(node, "expected an IR pattern")
        if introduce:
            self.anchors.add(info.type)
        return self.remember(node, info)

    def expression(self, node):
        if isinstance(node, ast.Name):
            if node.name in self.bindings:
                info = self.bindings[node.name]
            elif node.name in self.type_variables or node.name in CONCRETE_TYPES:
                info = ExprInfo("type", self.type_reference(node.name, node))
            else:
                self.error(node, f"undefined name {node.name!r}")
        elif isinstance(node, ast.Number):
            info = ExprInfo("constant", self.constraints.fresh())
        elif isinstance(node, ast.Unary):
            operand = self.expression(node.operand)
            self.require(operand, "condition" if node.operator == "not" else "constant", node)
            info = operand
        elif isinstance(node, ast.Binary):
            left = self.expression(node.left)
            right = self.expression(node.right)
            kind = "condition" if node.operator in {"and", "or"} else "constant"
            self.require(left, kind, node.left)
            self.require(right, kind, node.right)
            if kind == "constant":
                self.unify(left.type, right.type, node)
            info = ExprInfo(kind, left.type, left.dependencies | right.dependencies)
        elif isinstance(node, ast.Compare):
            operands = [self.expression(operand) for operand in node.operands]
            for left, right, op in zip(operands, operands[1:], node.operators):
                if left.kind != right.kind or left.kind not in {"constant", "type", "condition"}:
                    self.error(node, "comparisons require constants of the same type, "
                               "two types, or two conditions")
                if left.kind == "constant":
                    self.unify(left.type, right.type, node)
                elif op not in {"==", "!="}:
                    self.error(node, f"ordering is not defined for {left.kind}")
            info = ExprInfo("condition", dependencies=frozenset().union(
                *(operand.dependencies for operand in operands)))
        elif isinstance(node, ast.Call):
            info = self.call(node)
        else:
            self.error(node, "unsupported calculation expression")
        return self.remember(node, info)

    def call(self, node):
        arities = {"width": 1, "bits": 2, "min": 2, "max": 2, "ctz": 1,
                   "bit_reverse": 1, "mul_high_u": 2,
                   "known_zeros": 1, "known_ones": 1}
        if node.name not in arities:
            self.error(node, f"unknown calculation function {node.name!r}")
        if len(node.args) != arities[node.name]:
            self.error(node, f"{node.name} expects {arities[node.name]} arguments, "
                       f"got {len(node.args)}")
        args = [self.expression(arg) for arg in node.args]
        if node.name in KNOWN_BITS_FUNCTIONS:
            argument = node.args[0]
            if not isinstance(argument, ast.Name) or argument.name not in self.pattern_bindings:
                self.error(argument, f"{node.name} requires a matched value binding")
            return ExprInfo("constant", args[0].type,
                            frozenset((f"{node.name}({argument.name})",)))
        if node.name == "width":
            self.require(args[0], "type", node.args[0])
            return ExprInfo("constant", self.constraints.fresh())
        self.require(args[0], "constant", node.args[0])
        if node.name == "ctz":
            return ExprInfo("constant", self.constraints.fresh(), args[0].dependencies)
        if node.name == "bit_reverse":
            return ExprInfo("constant", args[0].type, args[0].dependencies)
        if node.name == "bits":
            self.require(args[1], "type", node.args[1])
            return ExprInfo("constant", args[1].type, args[0].dependencies)
        self.require(args[1], "constant", node.args[1])
        self.unify(args[0].type, args[1].type, node)
        return ExprInfo("constant", args[0].type, args[0].dependencies | args[1].dependencies)

    def check(self):
        root = self.pattern(self.rule.pattern, introduce=True)
        self.pattern_bindings = frozenset(self.bindings)
        for statement in self.rule.body:
            if isinstance(statement, ast.Binding):
                self.available_name(statement.name, statement)
                info = self.expression(statement.value)
                if info.kind == "value":
                    self.error(statement, "calculations cannot read runtime IR values")
                self.bindings[statement.name] = info
                self.definitions[statement.name] = statement.value
            else:
                info = self.expression(statement.value)
                self.require(info, "condition", statement)
                if statement.kind == "where" and info.dependencies:
                    names = ", ".join(sorted(info.dependencies))
                    self.error(statement, f"where depends on matched constants or analysis facts: {names}; use check")
        replacement = self.pattern(self.rule.replacement)
        self.unify(root.type, replacement.type, self.rule.replacement)
        anchors = {self.constraints.resolve(variable) for variable in self.anchors}
        replacement_types = {self.constraints.resolve(info.type)
                             for node, info in self.expressions.items()
                             if info.type is not None and self.is_replacement_node(node)}
        for node, info in self.expressions.items():
            if info.type is None:
                continue
            variable = self.constraints.resolve(info.type)
            domain = self.constraints.domains[variable.index]
            if variable not in anchors and len(domain) > 1:
                if variable in replacement_types:
                    self.error(node, "ambiguous replacement type; add a type annotation")
                self.constraints.restrict(variable, (INT64,))
        instantiations = []
        for assignment in self.constraints.assignments():
            if self.accepts_types(assignment):
                instantiations.append(assignment)
        if not instantiations:
            self.error(self.rule, "rule has no legal concrete type instantiations")
        representatives = {TypeVar(index): self.constraints.resolve(TypeVar(index))
                           for index in range(len(self.constraints.parents))}
        domains = {variable: tuple(type_ for type_ in CONCRETE_TYPES.values()
                                  if type_ in self.constraints.domains[variable.index])
                   for variable in representatives.values()}
        return CheckedRule(self.rule, self.bindings, self.type_variables,
                           self.expressions, tuple(instantiations), representatives, domains)

    def is_replacement_node(self, node):
        span = self.rule.replacement.span
        return span.start <= node.span.start and node.span.end <= span.end

    def accepts_types(self, assignment):
        for statement in self.rule.body:
            if isinstance(statement, ast.Condition) and statement.kind == "where":
                if not self.evaluate(statement.value, assignment):
                    return False
        return True

    def evaluate(self, node, assignment):
        if isinstance(node, ast.Number):
            result = node.value
        elif isinstance(node, ast.Name):
            if node.name in self.definitions:
                return self.evaluate(self.definitions[node.name], assignment)
            return assignment[self.expressions[node].type]
        elif isinstance(node, ast.Unary):
            result = UNARY_OPERATORS[node.operator](self.evaluate(node.operand, assignment))
        elif isinstance(node, ast.Binary):
            left = self.evaluate(node.left, assignment)
            if node.operator == "and":
                return left and self.evaluate(node.right, assignment)
            if node.operator == "or":
                return left or self.evaluate(node.right, assignment)
            right = self.evaluate(node.right, assignment)
            if node.operator in {"//", "%"} and right == 0:
                self.error(node, "division by zero in type condition")
            if node.operator in {"<<", ">>"}:
                width = assignment[self.expressions[node].type].width
                if right >= width:
                    self.error(node, "shift count exceeds type width in type condition")
            result = BINARY_OPERATORS[node.operator](left, right)
        elif isinstance(node, ast.Compare):
            left = self.evaluate(node.operands[0], assignment)
            for op, operand in zip(node.operators, node.operands[1:]):
                right = self.evaluate(operand, assignment)
                if not COMPARISON_OPERATORS[op](left, right):
                    return False
                left = right
            return True
        elif isinstance(node, ast.Call):
            args = [self.evaluate(arg, assignment) for arg in node.args]
            if node.name == "width":
                result = args[0].width
            elif node.name == "bits":
                result = args[0]
            elif node.name == "ctz":
                if args[0] == 0:
                    result = assignment[self.expressions[node.args[0]].type].width
                else:
                    result = (args[0] & -args[0]).bit_length() - 1
            elif node.name in CALCULATION_FUNCTIONS:
                width = assignment[self.expressions[node.args[0]].type].width
                result = CALCULATION_FUNCTIONS[node.name](*args, width)
            elif node.name == "min":
                result = min(args)
            else:
                result = max(args)
        else:
            self.error(node, "cannot evaluate type condition")
        info = self.expressions[node]
        if info.kind == "constant":
            result &= (1 << assignment[info.type].width) - 1
        return result


def check(source, filename="<string>"):
    checked = []
    names = set()
    for rule in parse(source, filename):
        if rule.name in names:
            raise TypeCheckError(f"duplicate rule name {rule.name!r}", rule.span, source, filename)
        names.add(rule.name)
        checked.append(Checker(rule, source, filename).check())
    return tuple(checked)
