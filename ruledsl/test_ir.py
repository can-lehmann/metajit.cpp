import unittest

from . import ir
from .types import INT32


class IRTests(unittest.TestCase):
    def test_bindings_with_same_name_hint_have_distinct_identities(self):
        first = ir.Value("arg")
        second = ir.Value("arg")
        self.assertNotEqual(first, second)
        self.assertEqual(len({first, second}), 2)
        block = ir.Block([
            ir.Bind(first, ir.Literal(1)),
            ir.Bind(second, ir.Binary("+", ir.Ref(first), ir.Literal(1))),
            ir.Return(ir.Ref(second)),
        ])
        ir.validate(block)

    def test_guarded_operand_read_and_successful_rewrite(self):
        input_ = ir.Value("arg")
        matched = ir.Value("add")
        operand = ir.Value("operand")
        result = ir.Value("result")
        block = ir.Block([
            ir.Bind(matched, ir.Call(ir.Symbol("as_add"), (ir.Ref(input_),))),
            ir.If(ir.Binary("!=", ir.Ref(matched), ir.Literal(None)), ir.Block([
                ir.Bind(operand, ir.Call(ir.Symbol("operand"), (ir.Ref(matched), ir.Literal(0)))),
                ir.Bind(result, ir.Call(ir.Symbol("fold_add"), (ir.Ref(operand), ir.Ref(operand)))),
                ir.ExprStmt(ir.Call(ir.Symbol("record_rule"), (ir.Literal(0),))),
                ir.Return(ir.Ref(result)),
            ])),
            ir.Return(ir.Literal(None)),
        ])
        ir.validate(block, (input_,))
        self.assertEqual([call.function.name for call in block.walk(ir.Call)],
                         ["as_add", "operand", "fold_add", "record_rule"])

    def test_walk_preserves_occurrences_order_and_branches(self):
        shared = ir.Literal(1)
        condition = ir.Binary("==", shared, shared)
        then_block = ir.Block([ir.Return(ir.Literal(INT32))])
        else_block = ir.Block([ir.Return(ir.Literal(None))])
        branch = ir.If(condition, then_block, else_block)
        block = ir.Block([branch])
        self.assertEqual(list(block.walk()), [
            block, branch, condition, shared, shared, then_block,
            then_block.statements[0], then_block.statements[0].expr,
            else_block, else_block.statements[0], else_block.statements[0].expr,
        ])
        self.assertEqual(len(list(block.walk(ir.Literal))), 4)

    def test_value_identities_are_not_syntax_children(self):
        value = ir.Value("arg")
        expression = ir.Ref(value)
        binding = ir.Bind(value, ir.Literal(0))
        self.assertEqual(list(expression.iter_children()), [])
        self.assertEqual(list(binding.iter_children()), [binding.expr])

    def test_call_callee_can_be_a_local_value(self):
        function = ir.Value("function")
        argument = ir.Value("argument")
        call = ir.Call(ir.Ref(function), (ir.Unary("~", ir.Ref(argument)),))
        ir.validate(ir.Block([ir.Return(call)]), (function, argument))
        with self.assertRaisesRegex(ir.InvalidIR, "unbound value 'function'"):
            ir.validate(ir.Block([ir.Return(call)]), (argument,))

    def test_else_branch_and_empty_return(self):
        condition = ir.Value("condition")
        block = ir.Block([ir.If(ir.Unary("not", ir.Ref(condition)), ir.Block([ir.Return()]),
                                ir.Block([ir.Return(ir.Literal(False))]))])
        ir.validate(block, (condition,))

    def test_branches_have_independent_local_scopes(self):
        condition = ir.Value("condition")
        first = ir.Value("arg")
        second = ir.Value("arg")
        block = ir.Block([ir.If(ir.Ref(condition),
                                ir.Block([ir.Bind(first, ir.Literal(1)), ir.Return(ir.Ref(first))]),
                                ir.Block([ir.Bind(second, ir.Literal(2)), ir.Return(ir.Ref(second))]))])
        ir.validate(block, (condition,))

    def test_branch_local_cannot_escape_to_parent(self):
        local = ir.Value("local")
        block = ir.Block([
            ir.If(ir.Literal(True), ir.Block([ir.Bind(local, ir.Literal(1))])),
            ir.Return(ir.Ref(local)),
        ])
        with self.assertRaisesRegex(ir.InvalidIR, "unbound value 'local'"):
            ir.validate(block)

    def test_then_local_cannot_escape_to_else(self):
        local = ir.Value("local")
        block = ir.Block([ir.If(ir.Literal(True),
                                ir.Block([ir.Bind(local, ir.Literal(1))]),
                                ir.Block([ir.Return(ir.Ref(local))]))])
        with self.assertRaisesRegex(ir.InvalidIR, "unbound value 'local'"):
            ir.validate(block)

    def test_binding_is_unavailable_in_its_own_initializer(self):
        value = ir.Value("value")
        with self.assertRaisesRegex(ir.InvalidIR, "unbound value 'value'"):
            ir.validate(ir.Block([ir.Bind(value, ir.Ref(value))]))

    def test_use_before_definition(self):
        value = ir.Value("value")
        block = ir.Block([ir.ExprStmt(ir.Call(ir.Symbol("consume"), (ir.Ref(value),))),
                          ir.Bind(value, ir.Literal(0))])
        with self.assertRaisesRegex(ir.InvalidIR, "unbound value 'value'"):
            ir.validate(block)

    def test_identity_can_only_be_defined_once(self):
        value = ir.Value("value")
        block = ir.Block([ir.Bind(value, ir.Literal(0)), ir.Bind(value, ir.Literal(1))])
        with self.assertRaisesRegex(ir.InvalidIR, "defined more than once"):
            ir.validate(block)
        with self.assertRaisesRegex(ir.InvalidIR, "defined more than once"):
            ir.validate(ir.Block([ir.Bind(value, ir.Literal(0))]), (value,))

    def test_identity_cannot_be_defined_in_both_branches(self):
        value = ir.Value("value")
        block = ir.Block([ir.If(ir.Literal(True),
                                ir.Block([ir.Bind(value, ir.Literal(1))]),
                                ir.Block([ir.Bind(value, ir.Literal(2))]))])
        with self.assertRaisesRegex(ir.InvalidIR, "defined more than once"):
            ir.validate(block)

    def test_duplicate_parameter_identity(self):
        parameter = ir.Value("parameter")
        with self.assertRaisesRegex(ir.InvalidIR, "duplicate parameter"):
            ir.validate(ir.Block(), (parameter, parameter))

    def test_validation_does_not_rewrite_the_tree(self):
        value = ir.Value("unused")
        binding = ir.Bind(value, ir.Literal(0))
        branch = ir.If(ir.Literal(True), ir.Block([ir.Return(ir.Literal(1))]))
        block = ir.Block([binding, branch, ir.Return(ir.Literal(None))])
        statements = list(block.statements)
        ir.validate(block)
        self.assertEqual(block.statements, statements)
        self.assertIs(block.statements[0], binding)
        self.assertIs(block.statements[1], branch)


if __name__ == "__main__":
    unittest.main()
