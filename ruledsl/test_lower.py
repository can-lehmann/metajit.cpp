from dataclasses import replace
import unittest

from . import check, ir
from .cpp import emit
from .lower import lower, operation_name
from .matcher import build_matchers


class LowerTests(unittest.TestCase):
    def lower(self, source, handwritten=()):
        return lower(build_matchers(check(source)), handwritten)

    def test_lowering_does_not_use_proof_instantiations(self):
        checked = check("zero: Add(x, 0)\n    => x")[0]
        program = lower(build_matchers((replace(checked, instantiations=()),)))
        self.assertEqual(len(program.functions), 1)
        self.assertEqual(program.functions[0].name, "fold_add")

    def test_fallback_dispatch_is_explicit(self):
        program = self.lower("zero: Add(x, 0)\n    => x", handwritten=("Add", "Sub"))
        self.assertEqual([function.name for function in program.functions], ["fold_add", "fold_sub"])
        for function, expected in zip(program.functions, ("_fold_add", "_fold_sub")):
            fallback = function.body.statements[-1]
            self.assertIsInstance(fallback, ir.Return)
            self.assertEqual(fallback.expr.function.name, expected)
        raw = self.lower("zero: Add(x, 0)\n    => x")
        self.assertEqual(raw.functions[0].body.statements[-1].expr.function.name, "build_add")

    def test_replacement_operations_get_folding_wrappers(self):
        program = self.lower("twice: Mul(x, 2)\n    => Shl(x, 1)")
        self.assertEqual([function.name for function in program.functions], ["fold_mul", "fold_shl"])
        calls = [call.function for call in program.functions[0].body.walk(ir.Call)]
        self.assertIn(ir.Symbol("fold_shl"), calls)
        self.assertEqual(program.functions[1].body.statements[-1].expr.function, ir.Symbol("build_shl"))

    def test_unknown_handwritten_registration(self):
        with self.assertRaisesRegex(ValueError, "unknown handwritten operations: Typo"):
            self.lower("", ("Typo",))

    def test_resize_has_explicit_type_parameter(self):
        program = self.lower("same: ResizeU(x: T): T\n    => x")
        self.assertEqual([parameter.kind for parameter in program.functions[0].parameters], ["value", "type"])

    def test_reuse_existing_operation_subtree(self):
        program = self.lower("absorb: And(x, And(x, y))\n    => And(x, y)")
        calls = [call.function for call in program.functions[0].body.walk(ir.Call)]
        self.assertNotIn(ir.Symbol("fold_and"), calls)

    def test_reuse_matching_input_literal(self):
        program = self.lower("ones: Or(x, -1)\n    => -1")
        calls = [call.function for call in program.functions[0].body.walk(ir.Call)]
        self.assertNotIn(ir.Symbol("build_const"), calls)


    def test_operation_names(self):
        for name, expected in (("ResizeU", "resize_u"), ("LtS", "lt_s"), ("Popcount", "popcount")):
            self.assertEqual(operation_name(name), expected)

    def test_no_ir_optimization(self):
        program = self.lower("same: Add(x, 0)\n    unused = 1\n    => x")
        binds = list(program.functions[0].body.walk(ir.Bind))
        self.assertTrue(any(bind.value.name_hint == "unused" for bind in binds))

    def test_nested_integer_signatures_eliminate_type_guards(self):
        source = "combine: Add(Add(x, C1), C2)\n    C = C1 + C2\n    => Add(x, C)"
        program = self.lower(source)
        code = emit(program)
        self.assertNotIn("is_int(", code)
        self.assertNotIn("is_int_or_bool(", code)
        self.assertEqual(list(program.functions[0].body.walk(ir.If)),
                         [branch for branch in program.functions[0].body.walk(ir.If)
                          if isinstance(branch.condition, ir.Binary) and branch.condition.operator == "!="])

    def test_unrestricted_jitir_domains_keep_dsl_domain_guard(self):
        sources = ("same: Eq(x, x)\n    => 1: Bool",
                   "same: Select(c, x, x)\n    => x",
                   "same: Freeze(x)\n    => x")
        for source in sources:
            with self.subTest(source=source):
                code = emit(self.lower(source))
                self.assertEqual(code.count("is_int_or_bool("), 1)
                self.assertNotIn("->type()) {", code)

    def test_nested_operation_can_restrict_unrestricted_domain(self):
        code = emit(self.lower("same: Freeze(Add(x, 0))\n    => x"))
        self.assertNotIn("is_int_or_bool(", code)
        self.assertNotIn("is_int(", code)

    def test_concrete_type_restriction_is_preserved(self):
        code = emit(self.lower("same: Eq(x: Bool, 1)\n    => x"))
        self.assertIn("= Type::Bool;", code)
        self.assertRegex(code, r"if \(type(?:_\d+)? == \(arg_0\)->type\(\)\)")
        self.assertNotIn("is_int_or_bool(", code)

    def test_resize_keeps_only_required_type_equality(self):
        program = self.lower("same: ResizeU(x: T): T\n    => x")
        branches = list(program.functions[0].body.walk(ir.If))
        self.assertEqual(len(branches), 1)
        self.assertEqual(branches[0].condition.operator, "==")
        self.assertNotIn("is_int_or_bool(", emit(program))

    def test_condition_materialization_is_explicit_in_ir(self):
        program = self.lower("eq: Eq(C1, C2)\n    C = C1 == C2\n    => C")
        conversions = list(program.functions[0].body.walk(ir.BoolToBits))
        self.assertTrue(conversions)
        self.assertTrue(all(isinstance(conversion.condition, ir.Ref) for conversion in conversions))
        self.assertIn("uint64_t(C)", emit(program))

    def test_condition_replacement_has_constant_priority(self):
        program = self.lower("identity: Eq(x: Bool, 1)\n    => x\n"
                             "constants: Eq(C1, C2)\n    C = C1 == C2\n    => C")
        comments = list(program.functions[0].body.walk(ir.Comment))
        self.assertTrue(comments[0].text.startswith("constants:"))

    def test_type_guards_do_not_leak_between_candidates(self):
        source = "boolean: Eq(x: Bool, 1)\n    => x\nsame: Eq(x, x)\n    => 1: Bool"
        code = emit(self.lower(source))
        self.assertIn("= Type::Bool;", code)
        self.assertRegex(code, r"if \(type(?:_\d+)? == \(arg_0\)->type\(\)\)")
        self.assertIn("is_int_or_bool(", code)


class CppEmitterTests(unittest.TestCase):
    def test_readable_rule_comments(self):
        source = "combine: Add(Add(x: T, C1), C2)\n    C = C1 + C2\n    => Add(x, C)"
        program = lower(build_matchers(check(source)))
        comments = list(program.functions[0].body.walk(ir.Comment))
        expected = "combine: Add(Add(x: T, C1), C2) => Add(x, C)"
        self.assertGreater(len(comments), 1)
        self.assertTrue(all(comment.text == expected for comment in comments))
        code = emit(program)
        self.assertEqual(code.count("// " + expected), len(comments))
        self.assertIn("// " + expected + "\n", code)

    def test_rule_comment_preserves_typed_replacement_and_negative_literal(self):
        source = "ones: Sub(0, 1): T\n    => -1: T"
        code = emit(lower(build_matchers(check(source))))
        self.assertIn("// ones: Sub(0, 1): T => -1: T\n", code)

    def test_name_allocation_avoids_keywords_external_names_and_collisions(self):
        program = lower(build_matchers(check("same: Add(x, 0)\n    => x")))
        function = program.functions[0]
        first, second, third = ir.Value("class"), ir.Value("class"), ir.Value("consume")
        body = ir.Block([ir.Bind(first, ir.Literal(1)), ir.Bind(second, ir.Ref(first)),
                         ir.Bind(third, ir.Ref(second)), ir.ExprStmt(ir.Call(ir.Symbol("consume"), (ir.Ref(third),))),
                         ir.Return(ir.Literal(None))])
        code = emit(replace(program, functions=(replace(function, body=body),)))
        self.assertIn("auto class_1 =", code)
        self.assertIn("auto class_2 =", code)
        self.assertIn("auto consume_1 =", code)
        self.assertIn("consume(consume_1);", code)

    def test_empty_program(self):
        self.assertEqual(emit(lower(build_matchers(()))), "")


if __name__ == "__main__":
    unittest.main()
