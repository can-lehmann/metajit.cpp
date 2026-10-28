import unittest

from . import TypeCheckError, check
from .types import BIT_TYPES, BOOL, INT8, INT16, INT32, INT64, INT_TYPES, TypeConflict, TypeConstraints


class TypeCheckTests(unittest.TestCase):
    def test_unified_domains_exist_only_at_roots(self):
        constraints = TypeConstraints()
        first = constraints.fresh((INT8, INT16))
        second = constraints.fresh((INT16, INT32))
        third = constraints.fresh()
        constraints.unify(first, second)
        constraints.unify(third, second)
        self.assertIsNone(constraints.domains[first.index])
        self.assertIsNone(constraints.domains[second.index])
        self.assertEqual(constraints.domains[third.index], {INT16})
        constraints.restrict(second, (INT16, INT64))
        constraints.unify(second, first)
        self.assertEqual(list(constraints.assignments()),
                         [{first: INT16, second: INT16, third: INT16}])
        self.assertEqual(constraints.resolve(second), third)
        self.assertIsNone(constraints.domains[second.index])

    def test_failed_unification_preserves_root_domains(self):
        constraints = TypeConstraints()
        first = constraints.fresh((INT8,))
        second = constraints.fresh((INT16,))
        with self.assertRaises(TypeConflict):
            constraints.unify(first, second)
        self.assertEqual(constraints.resolve(first), first)
        self.assertEqual(constraints.resolve(second), second)
        self.assertEqual(constraints.domains[first.index], {INT8})
        self.assertEqual(constraints.domains[second.index], {INT16})

    def test_bit_operation_calculations_preserve_types_and_dependencies(self):
        for operation, calculation, dependencies in (
                ("BitReverse(C)", "bit_reverse(C)", {"C"}),
                ("MulHighU(C1, C2)", "mul_high_u(C1, C2)", {"C1", "C2"})):
            checked = check(f"constant: {operation}\n    Cresult = {calculation}\n    => Cresult")[0]
            self.assertEqual(self.root_types(checked), list(INT_TYPES))
            self.assertEqual(checked.bindings["Cresult"].dependencies, frozenset(dependencies))
            for assignment in checked.instantiations:
                self.assertEqual(assignment[checked.bindings["Cresult"].type],
                                 checked.type_of(checked.rule.pattern, assignment))

    def test_bit_operation_helpers_in_type_conditions(self):
        checked = check("same: Add(x: T, 0)\n"
                        "    where bit_reverse(bits(1, T)) == 1 << (width(T) - 1)\n"
                        "    where mul_high_u(bits(-1, T), bits(-1, T)) == bits(-2, T)\n"
                        "    => x")[0]
        self.assertEqual(self.root_types(checked), list(INT_TYPES))

    def test_bit_operation_helpers_reject_wrong_arguments(self):
        for expression in ("bit_reverse(x)", "mul_high_u(x, 1)", "bit_reverse(1, 2)",
                           "mul_high_u(1)", "mul_high_u(bits(1, Int8), bits(1, Int16))"):
            with self.subTest(expression=expression), self.assertRaises(TypeCheckError):
                check(f"bad: Add(x, 0)\n    C = {expression}\n    => x")

    def checked(self, source):
        return check(source, "example.rules")[0]

    def root_types(self, checked):
        return [checked.type_of(checked.rule.pattern, assignment)
                for assignment in checked.instantiations]

    def test_infers_polymorphic_integer_rule(self):
        checked = self.checked("add_zero: Add(x, 0)\n    => x")
        self.assertEqual(self.root_types(checked), list(INT_TYPES))
        for assignment in checked.instantiations:
            self.assertEqual(checked.type_of(checked.rule.pattern, assignment),
                             checked.type_of(checked.rule.replacement, assignment))

    def test_boolean_bitwise_operations(self):
        checked = self.checked("and_self: And(x, x)\n    => x")
        self.assertEqual(self.root_types(checked), list(BIT_TYPES))

    def test_concrete_annotations(self):
        checked = self.checked("sub_self: Sub(x: Int32, x)\n    => 0")
        self.assertEqual(self.root_types(checked), [INT32])

    def test_later_annotations_constrain_earlier_uses(self):
        checked = self.checked("sub_self: Sub(x, x: Int16)\n    => 0")
        self.assertEqual(self.root_types(checked), [INT16])

    def test_replacement_annotation_constrains_rule(self):
        checked = self.checked("sub_self: Sub(x, x)\n    => 0: Int8")
        self.assertEqual(self.root_types(checked), [INT8])

    def test_type_variable_bindings_do_not_leak_between_rules(self):
        checked = check("a: Add(x: T, 0)\n    where T == Int8\n    => x\n"
                        "b: Add(x: T, 0)\n    where T == Int64\n    => x")
        self.assertEqual(self.root_types(checked[0]), [INT8])
        self.assertEqual(self.root_types(checked[1]), [INT64])

    def test_named_type_can_connect_separate_operands(self):
        checked = self.checked(
            "eq: Eq(ResizeU(x: T): U, ResizeU(y: T): U)\n    => Eq(x, y)"
        )
        self.assertEqual(len(checked.instantiations), 25)

    def test_fixed_intermediate_replacement_type(self):
        checked = self.checked(
            "id: Add(x: Int32, 0)\n    => ResizeU(ResizeU(x): Int8)"
        )
        inner = checked.rule.replacement.args[0]
        self.assertEqual(checked.type_of(inner, checked.instantiations[0]), INT8)

    def test_comparison_result_does_not_constrain_operands(self):
        checked = self.checked("eq_self: Eq(x, x)\n    => 1")
        self.assertEqual(len(checked.instantiations), 5)
        self.assertEqual(set(self.root_types(checked)), {BOOL})
        x = checked.rule.pattern.args[0]
        self.assertEqual({checked.type_of(x, assignment)
                          for assignment in checked.instantiations}, set(BIT_TYPES))

    def test_repeated_names_unify_different_operand_positions(self):
        checked = self.checked("select_self: Select(x, x, x)\n    => x")
        self.assertEqual(self.root_types(checked), [BOOL])

    def test_select_condition_is_bool(self):
        checked = self.checked("select_same: Select(cond, x, x)\n    => x")
        cond = checked.rule.pattern.args[0]
        for assignment in checked.instantiations:
            self.assertEqual(checked.type_of(cond, assignment), BOOL)
        self.assertEqual(self.root_types(checked), list(BIT_TYPES))

    def test_type_variables_and_width_filter(self):
        checked = self.checked(
            "resize: ResizeU(ResizeU(x: T): U): T\n"
            "    where width(U) >= width(T)\n    => x"
        )
        pairs = {(assignment[checked.type_variables["T"]],
                  assignment[checked.type_variables["U"]])
                 for assignment in checked.instantiations}
        self.assertEqual(pairs, {(t, u) for t in BIT_TYPES for u in BIT_TYPES
                                 if u.width >= t.width})
        self.assertEqual(len(pairs), 15)

    def test_type_equality_filters_without_unifying(self):
        checked = self.checked(
            "resize: ResizeU(x: T): U\n    where T != U\n    => ResizeS(x): U"
        )
        self.assertEqual(len(checked.instantiations), 20)

    def test_where_can_select_a_concrete_type(self):
        checked = self.checked("id: Add(x: T, 0)\n    where T == Int16\n    => x")
        self.assertEqual(self.root_types(checked), [INT16])

    def test_constant_calculations_inherit_operand_type(self):
        checked = self.checked(
            "reassoc: Add(Add(x, C1), C2)\n    C = C1 + C2\n    => Add(x, C)"
        )
        self.assertEqual(self.root_types(checked), list(INT_TYPES))
        self.assertEqual(checked.bindings["C"].kind, "constant")
        self.assertEqual(checked.bindings["C"].dependencies, frozenset(("C1", "C2")))
        for assignment in checked.instantiations:
            self.assertEqual(assignment[checked.bindings["C"].type],
                             checked.type_of(checked.rule.pattern, assignment))

    def test_bare_literal_binding_is_inferred_from_replacement(self):
        checked = self.checked("constant: Add(x: Int8, 0)\n    C = 1 + 2\n    => C")
        assignment = checked.instantiations[0]
        self.assertEqual(assignment[checked.bindings["C"].type], INT8)

    def test_ctz_has_independent_result_type_and_propagates_dependencies(self):
        checked = self.checked("count: ResizeU(C: Int8): Int64\n    count = ctz(C)\n    => count")
        assignment = checked.instantiations[0]
        self.assertEqual(assignment[checked.bindings["C"].type], INT8)
        self.assertEqual(assignment[checked.bindings["count"].type], INT64)
        self.assertEqual(checked.bindings["count"].dependencies, frozenset(("C",)))

    def test_ctz_in_type_conditions(self):
        checked = self.checked("same: Add(x, 0)\n    where ctz(8) == 3 and ctz(10) == 1 and ctz(0) == 64\n    => x")
        self.assertEqual(self.root_types(checked), list(INT_TYPES))

    def test_ctz_zero_uses_input_width_in_type_conditions(self):
        checked = self.checked("same: And(x: T, 0)\n    where ctz(bits(0, T)) == width(T)\n    => 0")
        self.assertEqual(self.root_types(checked), list(BIT_TYPES))

    def test_check_is_not_evaluated_during_type_enumeration(self):
        checked = self.checked("div_one: DivU(x, C)\n    check C == 1\n    => x")
        self.assertEqual(self.root_types(checked), list(INT_TYPES))

    def test_condition_bindings_propagate_dependencies(self):
        checked = self.checked(
            "id: Add(x, C)\n    is_zero = C == 0\n    check is_zero\n    => x"
        )
        self.assertEqual(checked.bindings["is_zero"].kind, "condition")
        self.assertEqual(checked.bindings["is_zero"].dependencies, frozenset(("C",)))

    def test_condition_materializes_as_bool_without_changing_binding_kind(self):
        checked = self.checked("eq: Eq(C1, C2)\n    C = C1 == C2\n    => C")
        self.assertEqual(checked.bindings["C"].kind, "condition")
        self.assertIsNone(checked.bindings["C"].type)
        self.assertEqual(checked.expressions[checked.rule.replacement].kind, "constant")
        for assignment in checked.instantiations:
            self.assertEqual(checked.type_of(checked.rule.replacement, assignment), BOOL)

    def test_condition_can_be_an_annotated_nested_bool_operand(self):
        checked = self.checked("select: Select(Eq(C1, C2), x, y)\n"
                               "    flag = C1 == C2\n    alias = flag\n"
                               "    check flag == alias\n    => Select(alias: Bool, x, y)")
        self.assertEqual(checked.bindings["alias"].kind, "condition")
        for assignment in checked.instantiations:
            self.assertEqual(checked.type_of(checked.rule.replacement.args[0], assignment), BOOL)

    def test_type_only_calculations_and_conditions(self):
        checked = self.checked(
            "id: Add(x: T, 0)\n    n = width(T)\n"
            "    small = n <= 16\n    where small\n    => x"
        )
        self.assertEqual(self.root_types(checked), [INT8, INT16])
        assignment = checked.instantiations[0]
        self.assertEqual(assignment[checked.bindings["n"].type], INT64)

    def test_type_alias(self):
        checked = self.checked(
            "id: Add(x: T, 0)\n    S = T\n    where width(S) == 32\n    => x"
        )
        self.assertEqual(self.root_types(checked), [INT32])

    def test_explicit_constant_conversion(self):
        checked = self.checked(
            "extend: ResizeU(C: Int8): Int64\n"
            "    wide = bits(C, Int64)\n    => wide"
        )
        self.assertEqual(self.root_types(checked), [INT64])
        self.assertEqual(checked.bindings["wide"].dependencies, frozenset(("C",)))

    def test_type_conditions_short_circuit(self):
        checked = self.checked(
            "id: Add(x: T, 0)\n    where width(T) == 8 or 1 // (width(T) - 8) == 0\n"
            "    => x"
        )
        self.assertEqual(self.root_types(checked), list(INT_TYPES))

    def test_chained_type_comparisons_and_precedence(self):
        checked = self.checked(
            "id: Add(x: T, 0)\n"
            "    where not width(T) == 32 and 8 <= width(T) < 64\n    => x"
        )
        self.assertEqual(self.root_types(checked), [INT8, INT16])

    def test_fixed_width_type_calculations(self):
        checked = self.checked(
            "id: Add(x: Int8, 0)\n    C = bits(255, Int8) + 1\n"
            "    where C == 0\n    => x"
        )
        self.assertEqual(self.root_types(checked), [INT8])

    def test_bitwise_and_helper_type_conditions(self):
        checked = self.checked(
            "id: Add(x: T, 0)\n"
            "    where width(T) & 31 == 0 and min(width(T), 32) == 32\n"
            "    where max(width(T), 32) >= 32 and ~(-1) == 0 and +1 == 1\n"
            "    => x"
        )
        self.assertEqual(self.root_types(checked), [INT32, INT64])

    def test_freeze_preserves_operand_type(self):
        checked = self.checked("freeze: Freeze(x: T)\n    => x")
        self.assertEqual(self.root_types(checked), list(BIT_TYPES))

    def test_diagnostic_has_source_location(self):
        source = "bad: Add(x: Int8, y: Int16)\n    => x"
        with self.assertRaises(TypeCheckError) as raised:
            check(source, "bad.rules")
        self.assertIn("bad.rules:1:19:", str(raised.exception))
        self.assertIn("incompatible types", str(raised.exception))
        self.assertIn("Int8", str(raised.exception))
        self.assertIn("Int16", str(raised.exception))

    def test_invalid_rules(self):
        cases = [
            ("bad: Add(x, 0)\n    => y", "undefined value 'y'"),
            ("bad: Add(x, C)\n    R = missing + C\n    => R", "undefined name 'missing'"),
            ("bad: Add(x, C)\n    C = C + 1\n    => x", "duplicate binding 'C'"),
            ("bad: Add(x, 0)\n    R = 1\n    R = 2\n    => R", "duplicate binding 'R'"),
            ("bad: Add(x: T, 0)\n    T = 1\n    => x", "already a type name"),
            ("bad: Add(Int8, 0)\n    => Int8", "already a type name"),
            ("bad: Add(x, 0)\n    R = S\n    S = 1\n    => R", "undefined name 'S'"),
            ("bad: Add(x, 0)\n    C = x + 1\n    => C", "expected constant, got value"),
            ("bad: Add(x, 0)\n    y = x\n    => y", "cannot read runtime IR values"),
            ("bad: Add(x, C)\n    where C == 0\n    => x", "use check"),
            ("bad: Add(x, C)\n    R = C + 1\n    where R == 0\n    => x", "use check"),
            ("bad: Add(x, 0)\n    check 1\n    => x", "expected condition"),
            ("bad: Add(x, 0)\n    check x == 0\n    => x", "comparisons require constants"),
            ("bad: Add(x, 0)\n    where width(Z) == 8\n    => x", "undefined name 'Z'"),
            ("bad: Add(x, 0)\n    where width(Int8) == 16\n    => x", "no legal concrete"),
            ("bad: Add(x, 0)\n    => ResizeU(x): U", "unknown type variable 'U'"),
            ("bad: Add(x, 0)\n    => ResizeU(ResizeU(x))", "ambiguous replacement type"),
            ("bad: Add(x, 0)\n    C = 1\n    => ResizeU(C)", "ambiguous replacement type"),
            ("bad: Add(x, 0)\n    => Eq(x, x)", "incompatible types"),
            ("bad: Add(x: Bool, 0)\n    => x", "incompatible types"),
            ("bad: Shl(x: Int8, C: Int16)\n    => x", "incompatible types"),
            ("bad: Add(x: Float32, 0)\n    => x", "unsupported concrete type"),
            ("bad: Add(x: Int33, 0)\n    => x", "unsupported concrete type"),
            ("bad: Load(x)\n    => x", "unknown or unsupported instruction"),
            ("bad: Add(x)\n    => x", "Add expects 2 operands"),
            ("bad: Add(x, 0)\n    C = width(x)\n    => C", "expected type, got value"),
            ("bad: Add(x, 0)\n    C = width(Int8, Int16)\n    => C", "width expects 1"),
            ("bad: Add(x, 0)\n    C = unsigned(1)\n    => C", "unknown calculation function"),
            ("bad: Add(x, C)\n    flag = C == 0\n    => flag", "incompatible types"),
            ("bad: Eq(C1, C2)\n    flag = C1 == C2\n    => flag: Int8", "incompatible types"),
            ("bad: Eq(C1, C2)\n    flag = C1 == C2\n    C = flag + 1\n    => C", "expected constant, got condition"),
            ("bad: Add(x: T, C)\n    C2 = T\n    => C2", "cannot use type"),
            ("bad: Add(x: T, 0)\n    where T < Int32\n    => x", "ordering is not defined"),
            ("bad: Add(x, 0)\n    where 1 // 0 == 0\n    => x", "division by zero"),
            ("bad: Add(x, 0)\n    where 1 << 64 == 0\n    => x", "shift count exceeds"),
            ("bad: Add(x, 0)\n    where 1 << -1 == 0\n    => x", "shift count exceeds"),
            ("bad: Add(x, C)\n    where ctz(C) == 0\n    => x", "use check"),
            ("bad: Add(x, 0)\n    where ctz(0) == 0\n    => x", "no legal concrete"),
            ("bad: Add(x, 0)\n    C = ctz(x)\n    => C", "expected constant, got value"),
            ("bad: Add(x, 0)\n    C = ctz(1, 2)\n    => C", "ctz expects 1"),
        ]
        for source, message in cases:
            with self.subTest(source=source):
                with self.assertRaises(TypeCheckError) as raised:
                    self.checked(source)
                self.assertIn(message, str(raised.exception))

    def test_duplicate_rule_names(self):
        source = "id: Add(x, 0)\n    => x\nid: Sub(x, 0)\n    => x"
        with self.assertRaisesRegex(TypeCheckError, "duplicate rule name 'id'"):
            check(source)


if __name__ == "__main__":
    unittest.main()
