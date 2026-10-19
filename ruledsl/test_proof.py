import unittest
from unittest.mock import patch

import z3

from .proof import ProofError, prove
from .semantics import Value, emit
from .types import BOOL, INT8, INT16


class SemanticsTests(unittest.TestCase):
    def test_signed_operations_and_wraparound(self):
        examples = [
            ("Add", 255, 1, 0), ("Sub", 0, 1, 255), ("Mul", 128, 2, 0),
            ("DivS", 249, 3, 254), ("ModS", 249, 3, 255),
            ("DivU", 249, 3, 83), ("ModU", 250, 3, 1),
            ("ShrS", 128, 1, 192), ("ShrU", 128, 1, 64),
            ("LtS", 255, 1, 1), ("LtU", 255, 1, 0), ("Eq", 1, 1, 1),
        ]
        for kind, a, b, expected in examples:
            with self.subTest(kind=kind):
                type_ = BOOL if kind in {"LtS", "LtU", "Eq"} else INT8
                result = emit(kind, [Value.constant(a, INT8), Value.constant(b, INT8)], type_)
                self.assertEqual(result.bits.as_long(), expected)
                self.assertTrue(z3.is_false(result.poison))

    def test_invalid_arithmetic_produces_poison(self):
        for kind, a, b in [
            ("DivS", 128, 255), ("ModS", 128, 255), ("DivU", 10, 0),
            ("ModU", 10, 0), ("DivS", 10, 0), ("ModS", 10, 0),
            ("Shl", 1, 8), ("ShrS", 1, 255), ("ShrU", 1, 8),
        ]:
            with self.subTest(kind=kind, a=a, b=b):
                result = emit(kind, [Value.constant(a, INT8), Value.constant(b, INT8)], INT8)
                self.assertTrue(z3.is_true(result.poison))

    def test_resize_signed_and_unsigned(self):
        for kind, expected in [("ResizeS", 65535), ("ResizeU", 255)]:
            with self.subTest(kind=kind):
                result = emit(kind, [Value.constant(255, INT8)], INT16)
                self.assertEqual(result.bits.as_long(), expected)

    def test_select_only_propagates_selected_arm_poison(self):
        poison = Value(z3.BitVecVal(0, 8), z3.BoolVal(True), INT8)
        defined = Value.constant(42, INT8)
        for condition, expected_poison in [(0, False), (1, True)]:
            result = emit("Select", [Value.constant(condition, BOOL), poison, defined], INT8)
            self.assertEqual(z3.is_true(result.poison), expected_poison)
        condition = Value(z3.BitVecVal(0, 1), z3.BoolVal(True), BOOL)
        self.assertTrue(z3.is_true(emit("Select", [condition, defined, defined], INT8).poison))

    def test_freeze_choices_are_stable_and_independent(self):
        input_ = Value(z3.BitVecVal(0, 8), z3.BoolVal(True), INT8)
        first = emit("Freeze", [input_], INT8)
        second = emit("Freeze", [input_], INT8)
        self.assertTrue(z3.is_false(first.poison))
        self.assertTrue(z3.is_true(z3.simplify(first.bits == first.bits)))
        solver = z3.Solver()
        solver.add(first.bits != second.bits)
        self.assertEqual(solver.check(), z3.sat)

    def test_resize_x_preserves_low_bits_with_independent_upper_bits(self):
        input_ = Value.constant(42, INT8)
        first = emit("ResizeX", [input_], INT16)
        second = emit("ResizeX", [input_], INT16)
        low = z3.simplify(z3.Extract(7, 0, first.bits))
        self.assertEqual(low.as_long(), 42)
        solver = z3.Solver()
        solver.add(first.bits != second.bits)
        self.assertEqual(solver.check(), z3.sat)


class ProofTests(unittest.TestCase):
    def test_basic_rules_all_instantiations(self):
        rules = prove(
            "add_zero: Add(x, 0)\n    => x\n"
            "sub_self: Sub(x, x)\n    => 0\n"
            "mul_zero: Mul(x, 0)\n    => 0\n"
            "sub_add: Sub(Add(x, y), y)\n    => x\n"
            "and_absorb: And(x, Or(x, y))\n    => x\n"
            "or_absorb: Or(x, And(x, y))\n    => x\n"
            "xor_self: Xor(x, x)\n    => 0\n"
            "eq_self: Eq(x, x)\n    => 1\n"
            "lt_self: LtU(x, x)\n    => 0\n"
            "select_same: Select(c, x, x)\n    => x\n"
        )
        self.assertEqual([len(rule.instantiations) for rule in rules], [4] * 4 + [5] * 4 + [4, 5])

    def test_reassociation_with_wrapping_constant_calculation(self):
        rules = prove("reassoc: Add(Add(x, C1), C2)\n    C = C1 + C2\n    => Add(x, C)")
        self.assertEqual(len(rules[0].instantiations), 4)

    def test_multiply_constant_selects(self):
        rules = prove(
            "multiply: Mul(Select(c, C1, C2), Select(c, C3, C4))\n"
            "    Ctrue = C1 * C3\n    Cfalse = C2 * C4\n"
            "    => Select(c, Ctrue, Cfalse)\n", timeout_ms=1000)
        self.assertEqual(len(rules[0].instantiations), 4)

    def test_select_case_split_preserves_counterexamples(self):
        for t, f, condition in (("+", "-", 0), ("-", "+", 1)):
            with self.subTest(condition=condition):
                with self.assertRaises(ProofError) as raised:
                    prove("bad: Add(Select(c, C1, C2), C3)\n"
                          f"    Ctrue = C1 {t} C3\n    Cfalse = C2 {f} C3\n"
                          "    => Select(c, Ctrue, Cfalse)\n")
                example = raised.exception.counterexample
                self.assertEqual(example.inputs["c"].value, condition)
                self.assertFalse(example.before.poison)
                self.assertNotEqual(example.before.value, example.after.value)

    def test_resize_roundtrips(self):
        for kind in ("ResizeU", "ResizeS", "ResizeX"):
            with self.subTest(kind=kind):
                rule = prove(
                    f"roundtrip: ResizeU({kind}(x: T): U): T\n"
                    "    where width(U) >= width(T)\n    => x"
                )[0]
                self.assertEqual(len(rule.instantiations), 15)

    def test_select_to_bitwise_rejects_new_poison(self):
        for pattern, replacement in (("Select(c, c, x)", "Or(c, x)"),
                                     ("Select(c, x, c)", "And(c, x)")):
            with self.subTest(pattern=pattern):
                with self.assertRaises(ProofError) as raised:
                    prove(f"bad: {pattern}\n    => {replacement}")
                example = raised.exception.counterexample
                self.assertFalse(example.before.poison)
                self.assertTrue(example.after.poison)
                self.assertTrue(example.inputs["x"].poison)

    def test_poison_removal_is_valid_refinement(self):
        prove("remove_poison: And(x, 0)\n    => 0")

    def test_freeze_constant(self):
        prove("freeze_constant: Freeze(C)\n    => C")

    def test_popcount(self):
        prove("count_zero: Popcount(And(x, 0))\n    => 0")

    def test_shift_fusion(self):
        prove(
            "fuse: Shl(Shl(x: T, C1), C2)\n"
            "    check C1 < width(T)\n    check C2 < width(T)\n"
            "    C = C1 + C2\n    check C < width(T)\n    => Shl(x, C)"
        )

    def test_explicit_constant_conversion(self):
        prove("extend: ResizeU(C: Int8): Int64\n    wide = bits(C, Int64)\n    => wide")

    def test_guarded_calculations(self):
        prove("divide: DivU(C1, C2)\n    check C2 != 0\n    C = C1 // C2\n    => C")
        prove("modulo: ModU(C1, C2)\n    check C2 != 0\n    C = C1 % C2\n    => C")
        prove("shift: ShrU(C1: T, C2)\n    check C2 < width(T)\n    C = C1 >> C2\n    => C")
        prove("shift: Shl(C1: T, C2)\n    check C2 < width(T)\n    C = C1 << C2\n    => C")

    def test_ctz_values_and_short_circuit_safety(self):
        prove("same: Add(x, 0)\n    check ctz(1) == 0 and ctz(8) == 3 and ctz(10) == 1\n"
              "    check ctz(0x8000000000000000) == 63\n    => x")
        prove("same: Add(x: T, C)\n    check C == 0 or ctz(C) < width(T)\n    => Add(x, C)")

    def test_ctz_zero_is_defined_without_a_preceding_guard(self):
        rules = prove("zero: Freeze(C: T)\n    count = ctz(C)\n"
                      "    check C == 0\n    check count == width(T)\n    => C")
        self.assertEqual(len(rules[0].instantiations), 5)

    def test_checks_and_chained_comparisons_short_circuit(self):
        prove(
            "id: Add(x: T, C)\n"
            "    check C == 0 or C != 0 and 1 // C <= 1\n"
            "    check not 1 > 2\n    => Add(x, C)"
        )
        prove("id: Add(x, C)\n    check 0 < C <= 1 // C\n    => Add(x, C)")

    def test_min_max_and_type_conditions(self):
        prove(
            "id: Add(x: T, C)\n    S = T\n"
            "    where S == T and T != Bool\n"
            "    small = min(C, 1)\n    large = max(C, 1)\n"
            "    check small <= large\n    => Add(x, C)"
        )

    def test_constant_predicates(self):
        prove("id: Add(x, C)\n    flag = C == 0\n    check flag == flag\n    => Add(x, C)")

    def test_condition_results_materialize_as_bool(self):
        for operation, comparison in (("Eq", "C1 == C2"), ("LtU", "C1 < C2")):
            with self.subTest(operation=operation):
                rules = prove(f"constants: {operation}(C1, C2)\n    C = {comparison}\n    => C")
                self.assertEqual(len(rules[0].instantiations), 5 if operation == "Eq" else 4)

    def test_condition_aliases_and_boolean_operators_in_replacements(self):
        prove("eq: Eq(C1, C2)\n    different = C1 != C2\n"
              "    same = not different\n    alias = same\n    => alias: Bool")
        prove("select: Select(Eq(C1, C2), x, y)\n"
              "    flag = C1 == C2 and not C1 != C2\n    => Select(flag, x, y)")
        prove("equal: Eq(C1, C2)\n    flag = C1 <= C2 <= C1\n    => flag")

    def test_incorrect_condition_result_has_bool_counterexample(self):
        with self.assertRaises(ProofError) as raised:
            prove("bad: Eq(C1, C2)\n    C = C1 != C2\n    => C")
        example = raised.exception.counterexample
        self.assertEqual(example.after.type, BOOL)
        self.assertIn(example.after.value, (0, 1))
        self.assertNotEqual(example.before.value, example.after.value)

    def test_counterexample_reports_types_inputs_and_outputs(self):
        source = "bad: Add(x: T, y)\n    => Sub(x, y)"
        with self.assertRaises(ProofError) as raised:
            prove(source, "bad.rules")
        error = raised.exception
        self.assertIn("bad.rules:1:1:", str(error))
        self.assertIn("T=Int8", str(error))
        self.assertIn("counterexample:", str(error))
        example = error.counterexample
        self.assertEqual(example.inputs["x"].type, INT8)
        self.assertFalse(example.before.poison)
        self.assertNotEqual(example.before.value, example.after.value)

    def test_select_cannot_introduce_poison_from_unselected_arm(self):
        with self.assertRaisesRegex(ProofError, "could not prove refinement") as raised:
            prove("bad: Select(0, y, x)\n    => Or(x, And(y, 0))")
        example = raised.exception.counterexample
        self.assertFalse(example.before.poison)
        self.assertTrue(example.after.poison)

    def test_freeze_cannot_be_removed_for_poisonous_input(self):
        with self.assertRaisesRegex(ProofError, "could not prove refinement") as raised:
            prove("bad: Freeze(x)\n    => x")
        self.assertTrue(raised.exception.counterexample.inputs["x"].poison)

    def test_independent_freezes_are_not_identical(self):
        with self.assertRaisesRegex(ProofError, "could not prove refinement"):
            prove("bad: Eq(Freeze(x), Freeze(x))\n    => 1")

    def test_resize_x_does_not_zero_extend(self):
        with self.assertRaisesRegex(ProofError, "could not prove refinement"):
            prove("bad: Eq(ResizeX(x: Int8): Int16, ResizeU(x): Int16)\n    => 1")

    def test_overshift_in_replacement_is_poison(self):
        with self.assertRaisesRegex(ProofError, "could not prove refinement") as raised:
            prove("bad: Add(x: T, 0)\n    => Shl(x, 64)")
        self.assertTrue(raised.exception.counterexample.after.poison)

    def test_signed_overflow_can_be_removed_but_not_introduced(self):
        prove("negate: DivS(x, -1)\n    => Sub(0, x)")
        with self.assertRaisesRegex(ProofError, "could not prove refinement") as raised:
            prove("bad: Sub(0, x)\n    => DivS(x, -1)")
        example = raised.exception.counterexample
        self.assertEqual(example.inputs["x"].value, 128)
        self.assertTrue(example.after.poison)

    def test_unsigned_calculation_comparisons(self):
        prove("id: Add(x: T, C)\n    check C >= 0 and C <= -1\n    => Add(x, C)")
        with self.assertRaisesRegex(ProofError, "checks cannot hold"):
            prove("bad: Add(x, C)\n    check C < 0\n    => x")

    def test_reject_unreachable_type_instantiation(self):
        with self.assertRaisesRegex(ProofError, "result=Int8.*checks cannot hold"):
            prove("sub: Sub(x: T, C)\n    check C > 255\n    D = -C\n    => Add(x, D)")
        proven = prove(
            "sub: Sub(x: T, C)\n    where width(T) > 8\n"
            "    check C > 255\n    D = -C\n    => Add(x, D)"
        )
        self.assertEqual(len(proven[0].instantiations), 3)

    def test_reject_always_poison_source(self):
        with self.assertRaisesRegex(ProofError, "source is always poison"):
            prove("bad: DivU(x, 0)\n    => 0")

    def test_unguarded_calculations_are_rejected(self):
        for expression in ("C1 // C2", "C1 % C2", "C1 << C2", "C1 >> C2"):
            with self.subTest(expression=expression):
                with self.assertRaisesRegex(ProofError, "calculation may be undefined"):
                    prove(f"bad: Add(C1, C2)\n    C = {expression}\n    => C")

    def test_later_guard_does_not_protect_earlier_calculation(self):
        with self.assertRaisesRegex(ProofError, "calculation may be undefined"):
            prove("bad: DivU(C1, C2)\n    C = C1 // C2\n    check C2 != 0\n    => C")

    def test_calculation_in_check_needs_its_own_guard(self):
        with self.assertRaisesRegex(ProofError, "calculation may be undefined"):
            prove("bad: Add(x, C)\n    check 1 // C == 0\n    => Add(x, C)")

    def test_unknown_is_not_a_successful_proof(self):
        with patch("ruledsl.proof.z3.Solver") as solver_class:
            solver = solver_class.return_value
            solver.check.return_value = z3.unknown
            solver.reason_unknown.return_value = "timeout"
            with self.assertRaisesRegex(ProofError, "Z3 could not decide the proof: timeout"):
                prove("id: Add(x, 0)\n    => x")

    def test_timeout_must_be_positive(self):
        with self.assertRaisesRegex(ValueError, "timeout must be positive"):
            prove("id: Add(x, 0)\n    => x", timeout_ms=0)


if __name__ == "__main__":
    unittest.main()
