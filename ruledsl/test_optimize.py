import unittest

from . import ir
from .generate import generate
from .optimize import optimize_block


class OptimizeTests(unittest.TestCase):
    def test_remove_tail_after_return(self):
        result = ir.Return(ir.Literal(1))
        block = ir.Block([result, ir.ExprStmt(ir.Call(ir.Symbol("unreachable"))), ir.Return()])
        optimized = optimize_block(block)
        self.assertEqual(optimized.statements, [result])
        self.assertEqual(len(block.statements), 3)

    def test_clean_both_branches_and_preserve_fallthrough(self):
        result = ir.Return(ir.Literal(1))
        branch = ir.If(ir.Symbol("condition"), ir.Block([result, ir.Return()]),
                       ir.Block([result, ir.Return()]))
        fallback = ir.Return(ir.Literal(None))
        optimized = optimize_block(ir.Block([branch, fallback]))
        self.assertEqual(optimized.statements[0].then_block.statements, [result])
        self.assertEqual(optimized.statements[0].else_block.statements, [result])
        self.assertIs(optimized.statements[1], fallback)
        ir.validate(optimized)

    def test_generation_cleans_both_backends(self):
        source = "constants: Add(C1, C2)\n    C = C1 + C2\n    => C\n"
        code = generate(source, simplify_source=source, include_simplify=True)
        self.assertEqual(code.count("// constants:"), 2)

    def test_guarded_permutations_remain(self):
        source = ("constants: Add(C1, C2)\n    check C1 < C2\n"
                  "    C = C1 + C2\n    => C\n")
        code = generate(source, simplify_source=source, include_simplify=True)
        self.assertEqual(code.count("// constants:"), 4)


if __name__ == "__main__":
    unittest.main()
