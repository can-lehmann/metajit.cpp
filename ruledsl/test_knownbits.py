from pathlib import Path
import unittest

import z3

from . import ast
from .generate import generate
from .matcher import build_matchers
from .proof import ProofError, Prover, prove
from .source import TypeCheckError
from .typecheck import check
from .types import BIT_TYPES


class KnownBitsTests(unittest.TestCase):
    def test_initial_rules_prove_at_all_widths(self):
        path = Path(__file__).with_name("simplify.rules")
        rules = prove(path.read_text(), str(path))
        self.assertEqual(len(rules), 8)
        self.assertTrue(all(rule.requires_facts for rule in rules))
        self.assertEqual([len(rule.instantiations) for rule in rules],
                         [5, 5, 5, 5, 5, 10, 10, 10])

    def test_masks_have_the_matched_type(self):
        rule, = check("mask: Or(x: T, 0)\n    C = known_zeros(x) | known_ones(x)\n    => C\n")
        self.assertTrue(rule.requires_facts)
        for assignment in rule.instantiations:
            expected = assignment[rule.type_variables["T"]]
            for node in rule.expressions:
                if isinstance(node, ast.Call) and node.name.startswith("known_"):
                    self.assertEqual(rule.type_of(node, assignment), expected)
            self.assertEqual(assignment[rule.bindings["C"].type], expected)
        self.assertEqual(len(rule.instantiations), len(BIT_TYPES))

    def test_invalid_queries_and_type_conditions(self):
        cases = [
            ("C = known_zeros()", "expects 1"),
            ("C = known_ones(x, x)", "expects 1"),
            ("C = known_zeros(1)", "matched value binding"),
            ("C = known_ones(T)", "matched value binding"),
            ("C1 = 1\n    C = known_zeros(C1)", "matched value binding"),
            ("where known_zeros(x) == 0", "use check"),
            ("C = known_ones(x)\n    where C == 0", "use check"),
        ]
        for body, message in cases:
            with self.subTest(body=body), self.assertRaisesRegex(TypeCheckError, message):
                check(f"bad: Or(x: T, 0)\n    {body}\n    => x\n")

    def test_fact_rules_do_not_enter_builder_matching(self):
        source = ("literal: Or(x, 0)\n    => x\n"
                  "fact: Or(x, y)\n    check known_ones(x) | known_zeros(y) == -1\n    => x\n")
        rules = prove(source)
        builder = build_matchers(rules, include_facts=False)
        self.assertEqual(builder.rule_names, ("literal", "fact"))
        self.assertEqual(len(builder.matchers["Or"].stages), 1)
        only_facts = build_matchers(rules[1:], include_facts=False)
        self.assertFalse(only_facts.matchers)
        code = generate(source, include_simplify=True)
        self.assertNotIn("known_ones", code[:code.index("struct RuledslUnknownBits")])
        self.assertIn("known_ones", code[code.index("struct RuledslUnknownBits"):])

    def test_distinct_sources_share_rule_ids(self):
        code = generate("literal: Or(x, 0)\n    => x\n", include_simplify=True,
                        simplify_source="fact: Or(x, y)\n"
                                        "    check known_ones(x) | known_zeros(y) == -1\n    => x\n")
        self.assertIn('"literal", "fact"', code)
        self.assertIn("++ruledsl_rule_counts[0x1ULL]", code)
        with self.assertRaisesRegex(ValueError, "duplicate rule names"):
            generate("same: Or(x, 0)\n    => x\n", include_simplify=True,
                     simplify_source="same: And(x, -1)\n    => x\n")

    def test_query_masks_are_sound_but_need_not_be_complete(self):
        source = "same: Or(x, 0)\n    C = known_zeros(x) | known_ones(x)\n    => x\n"
        checked, = check(source)
        for assignment in checked.instantiations:
            prover = Prover(checked, assignment, source, "<string>", 10000)
            prover.prove()
            zeros, ones = prover.known_bits["x"]
            x = prover.inputs["x"]
            self.assertEqual(prover.solver.check(z3.Not(x.poison), zeros == 0, ones == 0), z3.sat)
            self.assertEqual(prover.solver.check(zeros & ones != 0), z3.unsat)
            self.assertEqual(prover.solver.check(z3.Not(x.poison), x.bits & zeros != 0), z3.unsat)
            self.assertEqual(prover.solver.check(z3.Not(x.poison), x.bits & ones != ones), z3.unsat)
            self.assertEqual(prover.solver.check(x.poison, x.bits == 1, zeros == 1, ones == 0), z3.sat)

    def test_unselected_poison_cannot_be_returned_based_on_masks(self):
        source = ("bad: Select(c, x, y)\n"
                  "    check known_zeros(x) == -1 and known_zeros(y) == -1\n    => x\n")
        with self.assertRaisesRegex(ProofError, "could not prove refinement") as raised:
            prove(source)
        self.assertTrue(raised.exception.counterexample.inputs["x"].poison)
        self.assertIn("x", raised.exception.counterexample.known_bits)

    def test_masks_can_materialize_a_nonpoison_constant(self):
        source = ("zero: Select(c, x, y)\n"
                  "    check known_zeros(x) == -1 and known_zeros(y) == -1\n"
                  "    Czero = known_ones(x) & 0\n    => Czero\n")
        self.assertEqual(len(prove(source)[0].instantiations), 5)

    def test_freeze_does_not_establish_nonpoison_inputs(self):
        with self.assertRaisesRegex(ProofError, "could not prove refinement"):
            prove("bad: Freeze(x)\n    check known_zeros(x) == -1\n    => x\n")

    def test_masks_can_guard_calculations(self):
        source = ("same: Or(x, 0)\n    check known_ones(x) != 0\n"
                  "    C = 1 // known_ones(x)\n    => x\n")
        prove(source)
        with self.assertRaisesRegex(ProofError, "undefined before its guard"):
            prove(source.replace("    check known_ones(x) != 0\n", ""))
