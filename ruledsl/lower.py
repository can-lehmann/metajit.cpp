from dataclasses import dataclass
import re

from . import ast, ir, matcher
from .signatures import SIGNATURES
from .types import BIT_TYPES, BOOL, INT_TYPES
from .typefacts import TypeFacts
from .typecheck import KNOWN_BITS_FUNCTIONS


@dataclass(frozen=True)
class Parameter:
    value: ir.Value
    kind: str


@dataclass(frozen=True)
class Function:
    name: str
    parameters: tuple[Parameter, ...]
    body: ir.Block


@dataclass(frozen=True)
class Program:
    rule_names: tuple[str, ...]
    functions: tuple[Function, ...]


def operation_name(name):
    # convert camel-case operation names to snake case names: ResizeU -> resize_u
    return re.sub(r"(?<!^)(?=[A-Z])", "_", name).lower()


def call(name, *args):
    return ir.Call(ir.Symbol(name), tuple(args))


def member_call(object_, name, *args):
    return ir.Call(ir.Member(object_, name), tuple(args))


def conjunction(expressions):
    expressions = iter(expressions)
    result = next(expressions, ir.Literal(True))
    for expression in expressions:
        result = ir.Binary("and", result, expression)
    return result


class Lowerer:
    def __init__(self, name, handwritten):
        self.name = name
        self.handwritten = handwritten
        self.parameters = tuple(Parameter(ir.Value(f"arg_{index}"), "value")
                                for index in range(len(SIGNATURES[name].parameters)))
        self.paths = {(index,): ir.Ref(parameter.value)
                      for index, parameter in enumerate(self.parameters)}
        if name in {"ResizeU", "ResizeS", "ResizeX"}:
            parameter = Parameter(ir.Value("result_type"), "type")
            self.parameters += (parameter,)
            self.result_type = ir.Ref(parameter.value)
        elif name in {"Eq", "LtS", "LtU"}:
            self.result_type = ir.Literal(BOOL)
        else:
            self.result_type = member_call(self.paths[(1,) if name == "Select" else (0,)], "type")

    def bind(self, block, hint, expression):
        value = ir.Value(hint)
        block.statements.append(ir.Bind(value, expression))
        return ir.Ref(value)

    def guard(self, block, condition):
        then_block = ir.Block()
        block.statements.append(ir.If(condition, then_block))
        return then_block

    def path_type(self, path, paths):
        return member_call(paths[path], "type") if path else self.result_type

    def decision(self, tree, block, paths, constants):
        if tree is None:
            return
        if isinstance(tree, matcher.Terminal):
            for candidate in tree.candidates:
                self.candidate(candidate, block, paths, constants)
            return
        class_name = "Const" if isinstance(tree, matcher.IsConstant) else f"{tree.name}Inst"
        matched = self.bind(block, "matched", call(f"dynamic_cast<{class_name}*>", paths[tree.path]))
        yes = ir.Block()
        no = ir.Block()
        block.statements.append(ir.If(ir.Binary("!=", matched, ir.Literal(None)), yes, no))
        yes_paths = dict(paths)
        yes_constants = dict(constants)
        if isinstance(tree, matcher.IsConstant):
            yes_constants[tree.path] = matched
        else:
            for index in range(len(SIGNATURES[tree.name].parameters)):
                yes_paths[tree.path + (index,)] = self.bind(
                    yes, "operand", member_call(matched, "arg", ir.Literal(index)))
        self.decision(tree.yes, yes, yes_paths, yes_constants)
        self.decision(tree.no, no, paths, constants)
        self.decision(tree.next, block, paths, constants)

    def candidate(self, candidate, block, paths, constants):
        types = {}
        facts = TypeFacts(candidate.pattern)
        for binding in candidate.types:
            first = binding.paths[0]
            type_ = self.bind(block, "type", self.path_type(first, paths))
            types[binding.variable] = type_
            for path in binding.paths[1:]:
                if facts.resolve(first) != facts.resolve(path):
                    block = self.guard(block, ir.Binary("==", type_, self.path_type(path, paths)))
                    facts.unify(first, path)
            known_domain = facts.domain(first)
            if known_domain is not None and known_domain.issubset(binding.domain):
                continue
            if binding.domain == BIT_TYPES:
                condition = call("is_int_or_bool", type_)
            elif binding.domain == INT_TYPES:
                condition = call("is_int", type_)
            else:
                condition = ir.Literal(False)
                for concrete in binding.domain:
                    condition = ir.Binary("or", condition, ir.Binary("==", type_, ir.Literal(concrete)))
            block = self.guard(block, condition)
            facts.restrict(first, binding.domain)
        for equality in candidate.equalities:
            block = self.guard(block, call("is_always_equal", paths[equality.left], paths[equality.right]))
        for literal in candidate.literals:
            bits = member_call(constants[literal.path], "value")
            expected = ir.Binary("&", ir.Literal(literal.value),
                                 call("type_mask", self.path_type(literal.path, paths)))
            block = self.guard(block, ir.Binary("==", bits, expected))
        lowerer = RuleLowerer(self, candidate, types, paths)
        for binding in candidate.bindings:
            lowerer.values[binding.name] = paths[binding.path]
            if binding.constant:
                lowerer.calculations[binding.name] = self.bind(
                    block, binding.name, member_call(constants[binding.path], "value"))
        for statement in candidate.checked.rule.body:
            if isinstance(statement, ast.Condition) and statement.kind == "where":
                block = self.guard(block, lowerer.expression(statement.value, inline=True))
        for statement in candidate.checked.rule.body:
            if isinstance(statement, ast.Binding):
                expression = lowerer.expression(statement.value)
                lowerer.calculations[statement.name] = self.bind(block, statement.name, expression)
            elif statement.kind == "check":
                block = self.guard(block, lowerer.expression(statement.value))
        block.statements.append(ir.Comment(ast.format_rule(candidate.checked.rule)))
        result = lowerer.replacement(candidate.checked.rule.replacement, block)
        block.statements.append(ir.ExprStmt(call("ruledsl_record", ir.Literal(candidate.rule_id))))
        block.statements.append(ir.Return(result))

    def function(self, opcode_matcher):
        block = ir.Block()
        if opcode_matcher is not None:
            for stage in opcode_matcher.stages:
                self.decision(stage, block, self.paths, {})
        prefix = "_fold_" if self.name in self.handwritten else "build_"
        arguments = tuple(ir.Ref(parameter.value) for parameter in self.parameters)
        if SIGNATURES[self.name].commutative:
            left, right = arguments
            constant_left = call("dynamic_cast<Const*>", left)
            constant_right = call("dynamic_cast<Const*>", right)
            condition = conjunction((ir.Binary("!=", constant_left, ir.Literal(None)),
                                     ir.Binary("==", constant_right, ir.Literal(None))))
            fallback = self.guard(block, condition)
            fallback.statements.append(ir.Return(call(prefix + operation_name(self.name), right, left)))
        block.statements.append(ir.Return(call(prefix + operation_name(self.name), *arguments)))
        ir.validate(block, tuple(parameter.value for parameter in self.parameters))
        return Function("fold_" + operation_name(self.name), self.parameters, block)


class RuleLowerer:
    def __init__(self, parent, candidate, types, paths):
        self.parent = parent
        self.candidate = candidate
        self.checked = candidate.checked
        self.types = types
        self.paths = paths
        self.values = {}
        self.calculations = {}
        self.definitions = {statement.name: statement.value for statement in self.checked.rule.body
                            if isinstance(statement, ast.Binding)}

    def type_of(self, node):
        variable = self.checked.type_representatives[self.checked.expressions[node].type]
        if variable in self.types:
            return self.types[variable]
        domain = self.checked.type_domains[variable]
        if len(domain) != 1:
            raise ValueError("unbound symbolic type during lowering")
        return ir.Literal(domain[0])

    def mask(self, node, expression):
        if self.checked.expressions[node].kind == "constant":
            return ir.Binary("&", expression, call("type_mask", self.type_of(node)))
        return expression

    def expression(self, node, inline=False):
        if isinstance(node, ast.Name):
            if inline and node.name in self.definitions:
                return self.expression(self.definitions[node.name], inline=True)
            if node.name in self.calculations:
                return self.calculations[node.name]
            return self.type_of(node)
        if isinstance(node, ast.Number):
            expression = ir.Literal(node.value)
        elif isinstance(node, ast.Unary):
            expression = ir.Unary(node.operator, self.expression(node.operand, inline))
        elif isinstance(node, ast.Binary):
            expression = ir.Binary(node.operator, self.expression(node.left, inline),
                                   self.expression(node.right, inline))
        elif isinstance(node, ast.Compare):
            operands = [self.expression(operand, inline) for operand in node.operands]
            expression = conjunction(ir.Binary(operator, left, right)
                                     for operator, left, right in zip(node.operators, operands, operands[1:]))
        elif isinstance(node, ast.Call):
            if node.name in KNOWN_BITS_FUNCTIONS:
                if not hasattr(self.parent, "facts"):
                    raise ValueError("KnownBits queries require the simplification backend")
                return member_call(self.parent.facts, node.name, self.values[node.args[0].name])
            args = tuple(self.expression(arg, inline) for arg in node.args)
            if node.name == "width":
                expression = call("uint64_t", call("type_width", *args))
            elif node.name == "bits":
                expression = args[0]
            elif node.name == "bit_reverse":
                expression = call("metajit::bit_reverse", self.type_of(node), *args)
            elif node.name == "mul_high_u":
                product = ir.Binary("*", call("__uint128_t", args[0]), args[1])
                expression = call("uint64_t", ir.Binary(">>", product, call("type_width", self.type_of(node))))
            elif node.name == "ctz":
                expression = ir.Conditional(
                    ir.Binary("==", args[0], ir.Literal(0)),
                    call("uint64_t", call("type_width", self.type_of(node.args[0]))),
                    call("uint64_t", call("__builtin_ctzll", *args)))
            else:
                expression = call(f"std::{node.name}<uint64_t>", *args)
        else:
            raise ValueError(f"unsupported calculation {node!r}")
        return self.mask(node, expression)

    def replacement(self, node, block):
        if isinstance(node, ast.Typed):
            return self.replacement(node.value, block)
        if isinstance(node, ast.Name) and node.name in self.values:
            return self.values[node.name]
        if isinstance(node, (ast.Call, ast.Number)):
            target = matcher.normalize(node, self.checked, self.checked.type_representatives)
            for path, pattern in matcher.pattern_positions(self.candidate.pattern):
                if path and matcher.equivalent(pattern, target):
                    return self.paths[path]
        if isinstance(node, ast.Call):
            args = tuple(self.replacement(arg, block) for arg in node.args)
            if node.name in {"ResizeU", "ResizeS", "ResizeX"}:
                args += (self.type_of(node),)
            expression = call("fold_" + operation_name(node.name), *args)
        else:
            bits = self.calculations[node.name] if isinstance(node, ast.Name) else self.mask(node, ir.Literal(node.value))
            if isinstance(node, ast.Name) and self.checked.bindings[node.name].kind == "condition":
                bits = ir.BoolToBits(bits)
            expression = call("build_const", self.type_of(node), bits)
        return self.parent.bind(block, "result", expression)


def lower(program, handwritten=(), operations=()):
    handwritten = frozenset(handwritten)
    unknown = handwritten.difference(SIGNATURES)
    if unknown:
        raise ValueError(f"unknown handwritten operations: {', '.join(sorted(unknown))}")
    names = dict.fromkeys(program.matchers)
    for name in operations:
        names[name] = None
    for name in sorted(handwritten):
        names[name] = None
    for opcode_matcher in program.matchers.values():
        for stage in opcode_matcher.stages:
            for candidate in candidates(stage):
                for node in expression_nodes(candidate.checked.rule.replacement):
                    if isinstance(node, ast.Call):
                        names[node.name] = None
    functions = tuple(Lowerer(name, handwritten).function(program.matchers.get(name)) for name in names)
    return Program(program.rule_names, functions)


def candidates(tree):
    if tree is None:
        return
    if isinstance(tree, matcher.Terminal):
        yield from tree.candidates
    else:
        yield from candidates(tree.yes)
        yield from candidates(tree.no)
        yield from candidates(tree.next)


def expression_nodes(node):
    yield node
    if isinstance(node, ast.Typed):
        yield from expression_nodes(node.value)
    elif isinstance(node, ast.Call):
        for arg in node.args:
            yield from expression_nodes(arg)
