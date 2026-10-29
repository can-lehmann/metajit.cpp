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

from dataclasses import dataclass, replace
from itertools import product
import unittest

from . import ast, check
from .matcher import (
    IsConstant, IsOperation, Literal, Terminal, Variable,
    build_matchers, commutative_patterns, normalize,
)
from .signatures import SIGNATURES
from .types import BOOL, INT8, INT16, INT32, INT64


@dataclass(eq=False)
class Value:
    name: str
    type: object = INT32
    args: tuple = ()
    constant: int | None = None


def at(root, path):
    for index in path:
        root = root.args[index]
    return root


def same_value(left, right):
    return left is right or (
        left.type == right.type and left.constant is not None
        and right.constant is not None and left.constant == right.constant
    )


def walk(tree, root, trace):
    if tree is None:
        return
    if isinstance(tree, Terminal):
        yield from tree.candidates
        return
    value = at(root, tree.path)
    trace.append(tree)
    if isinstance(tree, IsConstant):
        matched = value.constant is not None
    else:
        matched = value.name == tree.name
    yield from walk(tree.yes if matched else tree.no, root, trace)
    yield from walk(tree.next, root, trace)


def structural_matches(program, root, trace=None):
    if trace is None:
        trace = []
    if root.name not in program.matchers:
        return
    for stage in program.matchers[root.name].stages:
        for candidate in walk(stage, root, trace):
            types = {}
            valid = True
            for binding in candidate.types:
                values = [at(root, path).type for path in binding.paths]
                if values[0] not in binding.domain or any(value != values[0] for value in values):
                    valid = False
                    break
                types[binding.variable] = values[0]
            if not valid:
                continue
            if any(not same_value(at(root, guard.left), at(root, guard.right))
                   for guard in candidate.equalities):
                continue
            if any(at(root, guard.path).constant != guard.value & (
                (1 << at(root, guard.path).type.width) - 1
            ) for guard in candidate.literals):
                continue
            values = {binding.name: at(root, binding.path) for binding in candidate.bindings}
            yield candidate, values, types


def first_match(program, root, accept=None, trace=None):
    for candidate, values, types in structural_matches(program, root, trace):
        if accept is None or accept(candidate, values, types):
            return candidate
    return None


def reference_pattern(node, value, checked, assignment, bindings):
    if value.type != checked.type_of(node, assignment):
        return
    if isinstance(node, ast.Typed):
        yield from reference_pattern(node.value, value, checked, assignment, bindings)
    elif isinstance(node, ast.Number):
        if value.constant == node.value & ((1 << value.type.width) - 1):
            yield bindings
    elif isinstance(node, ast.Name):
        if checked.bindings[node.name].kind == "constant" and value.constant is None:
            return
        if node.name in bindings:
            if same_value(bindings[node.name], value):
                yield bindings
        else:
            updated = dict(bindings)
            updated[node.name] = value
            yield updated
    elif isinstance(node, ast.Call) and node.name == value.name:
        arguments = [value.args]
        if SIGNATURES[node.name].commutative:
            arguments.append(value.args[::-1])
        for args in arguments:
            pending = [bindings]
            for child, arg in zip(node.args, args):
                pending = [updated for current in pending
                           for updated in reference_pattern(child, arg, checked, assignment, current)]
            yield from pending


def binding_key(rule_id, bindings):
    return rule_id, tuple(sorted((name, id(value)) for name, value in bindings.items()))


class MatcherTests(unittest.TestCase):
    def program(self, source):
        return build_matchers(check(source))

    def variants(self, source):
        checked = check(source)[0]
        return commutative_patterns(normalize(checked.rule.pattern, checked,
                                              checked.type_representatives))

    def test_simple_commutative_rule(self):
        program = self.program("add_zero: Add(x, 0)\n    => x")
        x = Value("input")
        zero = Value("Const", constant=0)
        for args in ((x, zero), (zero, x)):
            with self.subTest(args=args):
                candidate = first_match(program, Value("Add", args=args))
                self.assertEqual(candidate.checked.rule.name, "add_zero")
        self.assertIsNone(first_match(program, Value("Add", args=(x, x))))
        self.assertIsNone(first_match(program, Value("Sub", args=(x, zero))))

    def test_nested_commutative_variants(self):
        variants = self.variants(
            "reassoc: Add(Add(x, C1), C2)\n    C = C1 + C2\n    => Add(x, C)"
        )
        self.assertEqual(len(variants), 4)
        program = self.program(
            "reassoc: Add(Add(x, C1), C2)\n    C = C1 + C2\n    => Add(x, C)"
        )
        x = Value("input")
        c1 = Value("Const", constant=1)
        c2 = Value("Const", constant=2)
        for inner_args in ((x, c1), (c1, x)):
            inner = Value("Add", args=inner_args)
            for args in ((inner, c2), (c2, inner)):
                with self.subTest(args=args):
                    matches = list(structural_matches(program, Value("Add", args=args)))
                    self.assertEqual(len(matches), 1)
                    self.assertIs(matches[0][1]["x"], x)
                    self.assertIs(matches[0][1]["C1"], c1)
                    self.assertIs(matches[0][1]["C2"], c2)

    def test_symmetric_variants_are_deduplicated(self):
        self.assertEqual(len(self.variants("self: And(x, x)\n    => x")), 1)
        self.assertEqual(len(self.variants("zeros: Add(0, 0)\n    => 0")), 1)
        variants = self.variants("same: And(Or(x, x), Or(x, x))\n    => x")
        self.assertEqual(len(variants), 1)

    def test_noncommutative_operations_are_not_swapped(self):
        variants = self.variants("sub: Sub(x, 0)\n    => x")
        self.assertEqual(len(variants), 1)
        self.assertIsInstance(variants[0].args[0], Variable)
        self.assertIsInstance(variants[0].args[1], Literal)

    def test_result_priority_and_shared_rule_ids(self):
        program = self.program(
            "built: Add(x, 0)\n    => Sub(x, 0)\n"
            "value: Add(x, 0)\n    => x\n"
            "constant: Add(C, 0)\n    => C"
        )
        self.assertEqual(program.rule_names, ("built", "value", "constant"))
        zero = Value("Const", constant=0)
        constant = Value("Const", constant=7)
        self.assertEqual(first_match(program, Value("Add", args=(constant, zero))).rule_id, 2)
        self.assertEqual(first_match(program, Value("Add", args=(Value("input"), zero))).rule_id, 1)
        ids = {candidate.rule_id for candidate, values, types in structural_matches(
            program, Value("Add", args=(zero, zero)))}
        self.assertEqual(ids, {0, 1, 2})

    def test_specific_literals_precede_constant_variables(self):
        program = self.program(
            "generic: Add(x, C)\n    check C == 0\n    => x\n"
            "specific: Add(x, 0)\n    => x"
        )
        root = Value("Add", args=(Value("input"), Value("Const", constant=0)))
        self.assertEqual(first_match(program, root).checked.rule.name, "specific")

    def test_reused_operation_precedes_constant_reassociation(self):
        program = self.program("combine: Or(Or(x, C1), C2)\n    C = C1 | C2\n    => Or(x, C)\n"
                               "absorb: Or(Or(x, y), x)\n    => Or(x, y)")
        x = Value("input")
        c1 = Value("Const", constant=7)
        c2 = Value("Const", constant=7)
        inner = Value("Or", args=(x, c1))
        self.assertEqual(first_match(program, Value("Or", args=(inner, c2))).checked.rule.name,
                         "absorb")

    def test_file_order_breaks_leaf_ties(self):
        program = self.program("first: Add(x, 0)\n    => x\nsecond: Add(y, 0)\n    => y")
        root = Value("Add", args=(Value("input"), Value("Const", constant=0)))
        self.assertEqual(first_match(program, root).checked.rule.name, "first")

    def test_shared_constant_check_and_literal_fallback(self):
        program = self.program(
            "zero: Add(x, 0)\n    => x\n"
            "one: Add(x, 1)\n    => x"
        )
        root = Value("Add", args=(Value("input"), Value("Const", constant=1)))
        trace = []
        self.assertEqual(first_match(program, root, trace=trace).checked.rule.name, "one")
        self.assertEqual(sum(isinstance(node, IsConstant) and node.path == (1,)
                             for node in trace), 1)

    def test_shared_opcode_check(self):
        program = self.program(
            "left: Sub(Add(x, y), x)\n    => y\n"
            "right: Sub(Add(x, y), y)\n    => x"
        )
        x = Value("x")
        y = Value("y")
        root = Value("Sub", args=(Value("Add", args=(x, y)), y))
        trace = []
        self.assertIsNotNone(first_match(program, root, trace=trace))
        self.assertEqual(sum(isinstance(node, IsOperation) and node.path == (0,)
                             for node in trace), 1)

    def test_opcode_failure_does_not_read_nested_operands(self):
        program = self.program("cancel: Sub(Add(x, y), y)\n    => x")
        trace = []
        self.assertIsNone(first_match(program, Value("Sub", args=(Value("input"), Value("other"))),
                                     trace=trace))
        self.assertTrue(all(len(node.path) == 1 for node in trace))

    def test_different_nested_opcodes_and_wildcard_fallback(self):
        program = self.program(
            "add: Sub(Add(x, y), y)\n    => x\n"
            "xor: Sub(Xor(x, y), y)\n    => x\n"
            "any: Sub(x, C)\n    check C == 0\n    => x"
        )
        x = Value("x")
        y = Value("y")
        for operation, expected in (("Add", "add"), ("Xor", "xor")):
            root = Value("Sub", args=(Value(operation, args=(x, y)), y))
            self.assertEqual(first_match(program, root).checked.rule.name, expected)
        root = Value("Sub", args=(Value("Add", args=(x, y)), Value("Const", constant=0)))
        self.assertEqual(first_match(program, root).checked.rule.name, "any")

    def test_failed_rule_check_falls_through(self):
        program = self.program(
            "guarded: Sub(Add(x, C1), C2)\n    check C1 == C2\n    => x\n"
            "fallback: Sub(Add(x, C1), C2)\n    C = C1 - C2\n    => Add(x, C)"
        )

        def accept(candidate, values, types):
            return candidate.checked.rule.name != "guarded" or (
                values["C1"].constant == values["C2"].constant
            )

        root = Value("Sub", args=(Value("Add", args=(Value("input"), Value("Const", constant=1))),
                                  Value("Const", constant=2)))
        self.assertEqual(first_match(program, root, accept).checked.rule.name, "fallback")

    def test_repeated_values_use_identity_or_equal_constants(self):
        program = self.program("same: Sub(x, x)\n    => 0")
        x = Value("input")
        self.assertIsNotNone(first_match(program, Value("Sub", args=(x, x))))
        self.assertIsNone(first_match(program, Value("Sub", args=(x, Value("input")))))
        self.assertIsNotNone(first_match(program, Value("Sub", args=(
            Value("Const", constant=7), Value("Const", constant=7)))))
        self.assertIsNone(first_match(program, Value("Sub", args=(
            Value("Const", constant=7), Value("Const", constant=8)))))

    def test_repeated_named_constants(self):
        program = self.program("same: Sub(C, C)\n    => 0")
        self.assertIsNotNone(first_match(program, Value("Sub", args=(
            Value("Const", constant=7), Value("Const", constant=7)))))
        self.assertIsNone(first_match(program, Value("Sub", args=(Value("input"), Value("input")))))

    def test_literal_is_masked_to_matched_type(self):
        program = self.program("all_ones: And(x, -1)\n    => x")
        for type_ in (BOOL, INT8, INT16, INT32, INT64):
            with self.subTest(type=type_):
                constant = Value("Const", type_, constant=(1 << type_.width) - 1)
                root = Value("And", type_, (Value("input", type_), constant))
                self.assertIsNotNone(first_match(program, root))

    def test_concrete_type_filter_falls_through(self):
        program = self.program(
            "narrow: Add(x: Int8, 0)\n    => x\n"
            "generic: Add(x, 0)\n    => x"
        )
        for type_, expected in ((INT8, "narrow"), (INT32, "generic")):
            root = Value("Add", type_, (Value("input", type_), Value("Const", type_, constant=0)))
            self.assertEqual(first_match(program, root).checked.rule.name, expected)

    def test_type_equality_between_separate_conversions(self):
        program = self.program(
            "eq: Eq(ResizeU(x: T): U, ResizeU(y: T): U)\n    => Eq(x, y)"
        )
        left = Value("ResizeU", INT32, (Value("input", INT8),))
        right = Value("ResizeU", INT32, (Value("other", INT16),))
        self.assertIsNone(first_match(program, Value("Eq", BOOL, (left, right))))
        right = Value("ResizeU", INT32, (Value("other", INT8),))
        self.assertIsNotNone(first_match(program, Value("Eq", BOOL, (left, right))))

    def test_type_conditions_remain_symbolic(self):
        checked = check(
            "resize: ResizeU(ResizeU(x: T): U): T\n"
            "    where width(U) >= width(T)\n    => x"
        )[0]
        program = build_matchers((replace(checked, instantiations=()),))
        root = Value("ResizeU", INT8, (Value("ResizeU", INT32, (Value("input", INT8),)),))
        candidates = list(structural_matches(program, root))
        self.assertEqual(len(candidates), 1)
        candidate, values, types = candidates[0]
        self.assertEqual(len(candidate.types), 2)
        self.assertEqual(candidate.checked.rule.body[0].kind, "where")
        self.assertEqual(types[checked.type_representatives[checked.type_variables["T"]]], INT8)
        self.assertEqual(types[checked.type_representatives[checked.type_variables["U"]]], INT32)

        def accept(candidate, values, types):
            variables = candidate.checked.type_variables
            representatives = candidate.checked.type_representatives
            source_type = types[representatives[variables["T"]]]
            inner_type = types[representatives[variables["U"]]]
            return inner_type.width >= source_type.width

        self.assertIsNotNone(first_match(program, root, accept))
        narrowing = Value("ResizeU", INT32, (Value("ResizeU", INT8, (Value("input", INT32),)),))
        self.assertIsNone(first_match(program, narrowing, accept))

    def test_program_is_independent_of_concrete_instantiation_count(self):
        checked = check("zero: Add(x, 0)\n    => x")[0]
        full = build_matchers((checked,))
        stripped = build_matchers((replace(checked, instantiations=()),))
        root = Value("Add", args=(Value("input"), Value("Const", constant=0)))
        left = first_match(full, root)
        right = first_match(stripped, root)
        self.assertEqual(left.pattern, right.pattern)
        self.assertEqual(left.types, right.types)

    def test_empty_program(self):
        program = build_matchers(())
        self.assertEqual(program.rule_names, ())
        self.assertEqual(program.matchers, {})

    def test_rule_ids_are_global_across_opcodes(self):
        program = self.program("add: Add(x, 0)\n    => x\nsub: Sub(x, x)\n    => 0")
        x = Value("input")
        candidate = first_match(program, Value("Sub", args=(x, x)))
        self.assertEqual(candidate.rule_id, 1)
        self.assertEqual(program.rule_names[candidate.rule_id], "sub")

    def test_differential_against_recursive_matching(self):
        checked = check(
            "zero: Add(x, 0)\n    => x\n"
            "self: Sub(x, x)\n    => 0\n"
            "cancel: Sub(Add(x, y), y)\n    => x\n"
            "reassoc: Add(Add(x, C1), C2)\n    C = C1 + C2\n    => Add(x, C)\n"
            "absorb: And(x, Or(x, y))\n    => x\n"
            "typed: And(x: Int8, -1)\n    => x"
        )
        program = build_matchers(checked)
        for type_ in (INT8, INT32):
            x, y = Value("x", type_), Value("y", type_)
            atoms = [x, y, Value("Const", type_, constant=0), Value("Const", type_, constant=1)]
            inner = [Value(kind, type_, args) for kind in ("Add", "Or")
                     for args in product(atoms, repeat=2)]
            pairs = list(product(atoms, repeat=2))
            pairs.extend(product(inner, atoms))
            pairs.extend(product(atoms, inner))
            for kind in ("Add", "Sub", "And"):
                for args in pairs:
                    root = Value(kind, type_, args)
                    actual = {binding_key(candidate.rule_id, values)
                              for candidate, values, types in structural_matches(program, root)}
                    expected = set()
                    for rule_id, rule in enumerate(checked):
                        for assignment in rule.instantiations:
                            for bindings in reference_pattern(rule.rule.pattern, root, rule, assignment, {}):
                                expected.add(binding_key(rule_id, bindings))
                    self.assertEqual(actual, expected, (type_, kind, args))


if __name__ == "__main__":
    unittest.main()
