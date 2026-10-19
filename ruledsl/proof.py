from dataclasses import dataclass

import z3

from . import ast
from .semantics import Value, emit, resize_bits
from .source import SourceError
from .typecheck import check
from .types import ConcreteType


@dataclass(frozen=True)
class ConcreteValue:
    type: ConcreteType
    value: int
    poison: bool


@dataclass(frozen=True)
class Counterexample:
    inputs: dict[str, ConcreteValue]
    before: ConcreteValue
    after: ConcreteValue | None


@dataclass(frozen=True)
class Calculation:
    value: z3.ExprRef | ConcreteType
    valid: z3.BoolRef


class ProofError(SourceError):
    def __init__(self, message, node, source, filename, instantiation, counterexample=None):
        self.instantiation = instantiation
        self.counterexample = counterexample
        super().__init__(message, node.span, source, filename)


def concrete_value(value, model):
    return ConcreteValue(value.type, model.eval(value.bits, model_completion=True).as_long(),
                         z3.is_true(model.eval(value.poison, model_completion=True)))


def format_value(value):
    return f"{value.type.name} = {value.value}, poison={str(value.poison).lower()}"


class Prover:
    def __init__(self, checked, assignment, source, filename, timeout_ms):
        self.checked = checked
        self.assignment = assignment
        self.source = source
        self.filename = filename
        self.solver = z3.Solver()
        self.solver.set(timeout=timeout_ms)
        self.inputs = {}
        self.calculations = {}
        self.before = self.pattern(checked.rule.pattern, introduce=True)
        self.after = None
        self.conditions = z3.BoolVal(True)

    def type_of(self, node):
        return self.checked.type_of(node, self.assignment)

    def error(self, node, message, model=None):
        types = [f"result={self.before.type.name}"]
        types.extend(f"{name}={self.assignment[variable].name}"
                     for name, variable in self.checked.type_variables.items())
        message = f"rule {self.checked.rule.name!r} ({', '.join(types)}): {message}"
        counterexample = None
        if model is not None:
            inputs = {name: concrete_value(value, model) for name, value in self.inputs.items()}
            before = concrete_value(self.before, model)
            after = concrete_value(self.after, model) if self.after is not None else None
            counterexample = Counterexample(inputs, before, after)
            lines = [message, "counterexample:"]
            lines.extend(f"  {name}: {format_value(value)}" for name, value in inputs.items())
            lines.append(f"  source: {format_value(before)}")
            if after is not None:
                lines.append(f"  replacement: {format_value(after)}")
            message = "\n".join(lines)
        raise ProofError(message, node, self.source, self.filename,
                         self.assignment, counterexample)

    def query(self, node, *conditions):
        result = self.solver.check(*conditions)
        if result == z3.unknown:
            self.error(node, f"Z3 could not decide the proof: {self.solver.reason_unknown()}")
        return result

    def pattern(self, node, introduce=False):
        if isinstance(node, ast.Typed):
            return self.pattern(node.value, introduce)
        if isinstance(node, ast.Number):
            return Value.constant(node.value, self.type_of(node))
        if isinstance(node, ast.Name):
            if introduce and node.name not in self.inputs:
                info = self.checked.bindings[node.name]
                self.inputs[node.name] = Value.fresh(node.name, self.type_of(node),
                                                     info.kind != "constant")
            if node.name in self.inputs:
                return self.inputs[node.name]
            bits = self.calculations[node.name].value
            if self.checked.bindings[node.name].kind == "condition":
                bits = z3.If(bits, z3.BitVecVal(1, 1), z3.BitVecVal(0, 1))
            return Value(bits, z3.BoolVal(False), self.type_of(node))
        args = [self.pattern(arg, introduce) for arg in node.args]
        return emit(node.name, args, self.type_of(node))

    def expression(self, node):
        info = self.checked.expressions[node]
        valid = z3.BoolVal(True)
        if isinstance(node, ast.Number):
            value = z3.BitVecVal(node.value, self.type_of(node).width)
        elif isinstance(node, ast.Name):
            if node.name in self.calculations:
                return self.calculations[node.name]
            if info.kind == "type":
                value = self.type_of(node)
            else:
                value = self.inputs[node.name].bits
        elif isinstance(node, ast.Unary):
            operand = self.expression(node.operand)
            valid = operand.valid
            if node.operator == "not":
                value = z3.Not(operand.value)
            elif node.operator == "-":
                value = -operand.value
            elif node.operator == "~":
                value = ~operand.value
            else:
                value = operand.value
        elif isinstance(node, ast.Binary):
            left = self.expression(node.left)
            right = self.expression(node.right)
            a, b = left.value, right.value
            valid = z3.And(left.valid, right.valid)
            if node.operator == "and":
                value = z3.And(a, b)
                valid = z3.And(left.valid, z3.Implies(a, right.valid))
            elif node.operator == "or":
                value = z3.Or(a, b)
                valid = z3.And(left.valid, z3.Implies(z3.Not(a), right.valid))
            elif node.operator == "+":
                value = a + b
            elif node.operator == "-":
                value = a - b
            elif node.operator == "*":
                value = a * b
            elif node.operator == "&":
                value = a & b
            elif node.operator == "|":
                value = a | b
            elif node.operator == "^":
                value = a ^ b
            elif node.operator == "//":
                value = z3.UDiv(a, b)
                valid = z3.And(valid, b != 0)
            elif node.operator == "%":
                value = z3.URem(a, b)
                valid = z3.And(valid, b != 0)
            elif node.operator == "<<":
                value = a << b
                valid = z3.And(valid, z3.ULT(b, a.size()))
            elif node.operator == ">>":
                value = z3.LShR(a, b)
                valid = z3.And(valid, z3.ULT(b, a.size()))
            else:
                self.error(node, f"unsupported calculation operator {node.operator!r}")
        elif isinstance(node, ast.Compare):
            left = self.expression(node.operands[0])
            valid = left.valid
            value = z3.BoolVal(True)
            for op, operand in zip(node.operators, node.operands[1:]):
                right = self.expression(operand)
                valid = z3.And(valid, z3.Implies(value, right.valid))
                condition = self.compare(op, left.value, right.value)
                value = z3.And(value, condition)
                left = right
        elif isinstance(node, ast.Call):
            args = [self.expression(arg) for arg in node.args]
            valid = z3.And(*(arg.valid for arg in args))
            if node.name == "width":
                value = z3.BitVecVal(args[0].value.width, self.type_of(node).width)
            elif node.name == "bits":
                value = resize_bits(args[0].value, self.type_of(node).width)
            elif node.name == "ctz":
                bits = args[0].value
                width = self.type_of(node).width
                value = z3.BitVecVal(bits.size(), width)
                for index in range(bits.size() - 1, -1, -1):
                    value = z3.If(z3.Extract(index, index, bits) == 1,
                                  z3.BitVecVal(index, width), value)
            elif node.name == "min":
                value = z3.If(z3.ULE(args[0].value, args[1].value), args[0].value, args[1].value)
            elif node.name == "max":
                value = z3.If(z3.UGE(args[0].value, args[1].value), args[0].value, args[1].value)
            else:
                self.error(node, f"unsupported calculation function {node.name!r}")
        else:
            self.error(node, "unsupported calculation expression")
        return Calculation(value, z3.simplify(valid))

    def compare(self, op, left, right):
        if op == "==":
            return z3.BoolVal(left == right) if isinstance(left, ConcreteType) else left == right
        if op == "!=":
            return z3.BoolVal(left != right) if isinstance(left, ConcreteType) else left != right
        if op == "<":
            return z3.ULT(left, right)
        if op == "<=":
            return z3.ULE(left, right)
        if op == ">":
            return z3.UGT(left, right)
        if op == ">=":
            return z3.UGE(left, right)
        raise ValueError(op)

    def prove(self):
        for statement in self.checked.rule.body:
            if isinstance(statement, ast.Condition) and statement.kind == "where":
                continue
            calculation = self.expression(statement.value)
            if self.query(statement, self.conditions, z3.Not(calculation.valid)) == z3.sat:
                self.error(statement, "calculation may be undefined before its guard", self.solver.model())
            if isinstance(statement, ast.Binding):
                self.calculations[statement.name] = calculation
            else:
                self.conditions = z3.And(self.conditions, calculation.value)
        if self.query(self.checked.rule, self.conditions) == z3.unsat:
            self.error(self.checked.rule, "checks cannot hold for this type instantiation; "
                       "restrict the types with where")
        if self.query(self.checked.rule, self.conditions, z3.Not(self.before.poison)) == z3.unsat:
            self.error(self.checked.rule, "source is always poison under its checks for this type "
                       "instantiation; restrict the types with where")
        self.after = self.pattern(self.checked.rule.replacement)
        violation = z3.Or(self.after.poison, self.before.bits != self.after.bits)
        if self.query(self.checked.rule, self.conditions, z3.Not(self.before.poison), violation) == z3.sat:
            self.error(self.checked.rule, "could not prove refinement", self.solver.model())


def prove(source, filename="<string>", timeout_ms=120000):
    if timeout_ms <= 0:
        raise ValueError("proof timeout must be positive")
    checked_rules = check(source, filename)
    for checked in checked_rules:
        for assignment in checked.instantiations:
            Prover(checked, assignment, source, filename, timeout_ms).prove()
    return checked_rules
