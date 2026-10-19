from dataclasses import dataclass, replace
from itertools import product

from . import ast
from .signatures import SIGNATURES
from .typecheck import CheckedRule
from .types import ConcreteType, TypeVar


Path = tuple[int, ...]


@dataclass(frozen=True)
class Pattern:
    type: TypeVar


@dataclass(frozen=True)
class Variable(Pattern):
    name: str
    constant: bool


@dataclass(frozen=True)
class Literal(Pattern):
    value: int


@dataclass(frozen=True)
class Operation(Pattern):
    name: str
    args: tuple[Pattern, ...]


@dataclass(frozen=True)
class Binding:
    name: str
    path: Path
    constant: bool


@dataclass(frozen=True)
class SameValue:
    left: Path
    right: Path


@dataclass(frozen=True)
class LiteralValue:
    path: Path
    value: int


@dataclass(frozen=True)
class TypeBinding:
    variable: TypeVar
    paths: tuple[Path, ...]
    domain: tuple[ConcreteType, ...]


@dataclass(frozen=True)
class Candidate:
    rule_id: int
    checked: CheckedRule
    pattern: Operation
    bindings: tuple[Binding, ...]
    equalities: tuple[SameValue, ...]
    literals: tuple[LiteralValue, ...]
    types: tuple[TypeBinding, ...]


@dataclass(frozen=True)
class Terminal:
    candidates: tuple[Candidate, ...]


@dataclass(frozen=True)
class IsConstant:
    path: Path
    yes: "Decision | None"
    no: "Decision | None"
    next: "Decision | None"


@dataclass(frozen=True)
class IsOperation:
    path: Path
    name: str
    yes: "Decision | None"
    no: "Decision | None"
    next: "Decision | None"


Decision = Terminal | IsConstant | IsOperation


@dataclass(frozen=True)
class OpcodeMatcher:
    name: str
    stages: tuple[Decision, ...]


@dataclass(frozen=True)
class MatcherProgram:
    rule_names: tuple[str, ...]
    matchers: dict[str, OpcodeMatcher]


def normalize(node, checked, variables):
    type_ = variables[checked.expressions[node].type]
    if isinstance(node, ast.Typed):
        return normalize(node.value, checked, variables)
    if isinstance(node, ast.Name):
        return Variable(type_, node.name, checked.bindings[node.name].kind in {"constant", "condition"})
    if isinstance(node, ast.Number):
        return Literal(type_, node.value)
    if isinstance(node, ast.Call):
        return Operation(type_, node.name, tuple(normalize(arg, checked, variables)
                                                for arg in node.args))
    raise ValueError(f"unsupported pattern {node!r}")


def commutative_patterns(pattern):
    if not isinstance(pattern, Operation):
        return (pattern,)
    variants = {}
    arguments = [commutative_patterns(arg) for arg in pattern.args]
    for args in product(*arguments):
        variant = replace(pattern, args=args)
        variants[variant] = None
        if SIGNATURES[pattern.name].commutative:
            variants[replace(pattern, args=args[::-1])] = None
    return tuple(variants)


def pattern_positions(pattern, path=()):
    yield path, pattern
    if isinstance(pattern, Operation):
        for index, arg in enumerate(pattern.args):
            yield from pattern_positions(arg, path + (index,))


def make_candidate(rule_id, checked, pattern):
    bindings = {}
    equalities = []
    literals = []
    positions = tuple(pattern_positions(pattern))
    for path, node in positions:
        if isinstance(node, Variable):
            if node.name in bindings:
                equalities.append(SameValue(bindings[node.name].path, path))
            else:
                bindings[node.name] = Binding(node.name, path, node.constant)
        elif isinstance(node, Literal):
            literals.append(LiteralValue(path, node.value))
    type_paths = {}
    for path, node in positions:
        if node.type not in type_paths:
            type_paths[node.type] = []
        type_paths[node.type].append(path)
    types = tuple(TypeBinding(variable, tuple(paths), checked.type_domains[variable])
                  for variable, paths in type_paths.items())
    return Candidate(rule_id, checked, pattern, tuple(bindings.values()), tuple(equalities),
                     tuple(literals), types)


def result_priority(checked):
    replacement = checked.rule.replacement
    while isinstance(replacement, ast.Typed):
        replacement = replacement.value
    if isinstance(replacement, ast.Number):
        return 0
    if isinstance(replacement, ast.Name):
        return 0 if checked.bindings[replacement.name].kind in {"constant", "condition"} else 1
    variables = checked.type_representatives
    target = normalize(replacement, checked, variables)
    pattern = normalize(checked.rule.pattern, checked, variables)
    if any(path and equivalent(node, target) for path, node in pattern_positions(pattern)):
        return 1
    return 2


def equivalent(left, right):
    if type(left) is not type(right):
        return False
    if not isinstance(left, Operation):
        return left == right
    if left.name != right.name or left.type != right.type:
        return False
    if all(equivalent(a, b) for a, b in zip(left.args, right.args)):
        return True
    return SIGNATURES[left.name].commutative and all(
        equivalent(a, b) for a, b in zip(left.args, right.args[::-1])
    )


def specificity(candidate):
    score = len(candidate.equalities)
    for path, node in pattern_positions(candidate.pattern):
        if isinstance(node, Literal):
            score += 2
        elif isinstance(node, Operation) or isinstance(node, Variable) and node.constant:
            score += 1
    return -score


@dataclass(frozen=True)
class Row:
    candidate: Candidate
    patterns: tuple[Pattern, ...]


def matches_constant(pattern):
    return isinstance(pattern, Literal) or isinstance(pattern, Variable) and pattern.constant


def decision_tree(rows, paths):
    if not rows:
        return None
    while paths:
        if any(matches_constant(row.patterns[0]) for row in rows):
            yes = []
            no = []
            rest = []
            for row in rows:
                if matches_constant(row.patterns[0]):
                    yes.append(Row(row.candidate, row.patterns[1:]))
                elif isinstance(row.patterns[0], Operation):
                    no.append(row)
                else:
                    rest.append(row)
            return IsConstant(paths[0], decision_tree(yes, paths[1:]),
                              decision_tree(no, paths), decision_tree(rest, paths))
        operations = [row.patterns[0] for row in rows if isinstance(row.patterns[0], Operation)]
        if operations:
            operation = operations[0]
            yes = []
            no = []
            rest = []
            for row in rows:
                pattern = row.patterns[0]
                if isinstance(pattern, Operation):
                    if pattern.name == operation.name:
                        yes.append(Row(row.candidate, pattern.args + row.patterns[1:]))
                    else:
                        no.append(row)
                else:
                    rest.append(row)
            children = tuple(paths[0] + (index,) for index in range(len(operation.args)))
            return IsOperation(paths[0], operation.name,
                               decision_tree(yes, children + paths[1:]),
                               decision_tree(no, paths), decision_tree(rest, paths))
        rows = [Row(row.candidate, row.patterns[1:]) for row in rows]
        paths = paths[1:]
    return Terminal(tuple(row.candidate for row in rows))


def build_matchers(checked_rules, include_facts=True):
    checked_rules = tuple(checked_rules)
    per_opcode = {}
    for rule_id, checked in enumerate(checked_rules):
        if checked.requires_facts and not include_facts:
            continue
        variables = checked.type_representatives
        pattern = normalize(checked.rule.pattern, checked, variables)
        if not isinstance(pattern, Operation):
            raise ValueError("matcher root must be an operation")
        if pattern.name not in per_opcode:
            per_opcode[pattern.name] = [[], [], []]
        priority = result_priority(checked)
        for variant in commutative_patterns(pattern):
            per_opcode[pattern.name][priority].append(make_candidate(rule_id, checked, variant))
    matchers = {}
    for name, groups in per_opcode.items():
        stages = []
        for candidates in groups:
            if not candidates:
                continue
            candidates.sort(key=specificity)
            rows = [Row(candidate, candidate.pattern.args) for candidate in candidates]
            paths = tuple((index,) for index in range(len(rows[0].patterns)))
            stages.append(decision_tree(rows, paths))
        matchers[name] = OpcodeMatcher(name, tuple(stages))
    return MatcherProgram(tuple(checked.rule.name for checked in checked_rules), matchers)
