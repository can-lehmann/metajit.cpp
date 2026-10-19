import unittest

from . import ParseError, ast, parse
from .lexer import tokenize


class LexerTests(unittest.TestCase):
    def test_longest_operators_and_signed_numbers(self):
        tokens = tokenize("=> == != <= >= << >> // -0xff +0b10")
        self.assertEqual(
            [(token.kind, token.text) for token in tokens[:-2]],
            [(text, text) for text in ("=>", "==", "!=", "<=", ">=", "<<", ">>", "//", "-")]
            + [("NUMBER", "0xff"), ("+", "+"), ("NUMBER", "0b10")],
        )

    def test_keywords_have_identifier_boundaries(self):
        tokens = tokenize("check checker where whereabouts and android or orbit not nothing")
        self.assertEqual([token.kind for token in tokens[:-2]],
                         ["check", "NAME", "where", "NAME", "and", "NAME",
                          "or", "NAME", "not", "NAME"])

    def test_invalid_character_is_not_skipped(self):
        with self.assertRaises(ParseError) as raised:
            tokenize("Add(x, @y)", "example.rules")
        self.assertEqual(raised.exception.span.start, 7)
        self.assertIn("example.rules:1:8: unexpected character '@'", str(raised.exception))

    def test_indentation_and_comments(self):
        tokens = tokenize("a\n    # ignored\n    b # inline\n\n    c\nd")
        self.assertEqual([token.kind for token in tokens],
                         ["NAME", "NEWLINE", "INDENT", "NAME", "NEWLINE",
                          "NAME", "NEWLINE", "DEDENT", "NAME", "NEWLINE", "EOF"])

    def test_indentation_errors(self):
        for source, message in [
            ("a\n\tb", "use spaces for indentation"),
            ("a\n    b\n  c", "inconsistent indentation"),
        ]:
            with self.subTest(source=source):
                with self.assertRaisesRegex(ParseError, message):
                    tokenize(source)

    def test_windows_line_endings_and_offsets(self):
        source = "a\r\n    b\r\n"
        tokens = tokenize(source)
        name = tokens[3]
        self.assertEqual((name.span.line, name.span.column), (2, 5))
        self.assertEqual(source[name.span.start:name.span.end], "b")


class ParserTests(unittest.TestCase):
    def calculation(self, expression):
        return parse(f"example: Add(x, C)\n    R = {expression}\n    => R\n")[0].body[0].value

    def test_simple_rules_and_final_newline(self):
        for ending in ("", "\n", "\r\n"):
            with self.subTest(ending=ending):
                rules = parse("add_zero: Add(x, 0)\n    => x" + ending)
                self.assertEqual(len(rules), 1)
                rule = rules[0]
                self.assertEqual(rule.name, "add_zero")
                self.assertEqual(rule.pattern.name, "Add")
                self.assertEqual(rule.pattern.args[0].name, "x")
                self.assertEqual(rule.pattern.args[1].value, 0)
                self.assertEqual(rule.replacement.name, "x")
                self.assertEqual(rule.body, ())

    def test_multiple_rules_blank_lines_and_comments(self):
        rules = parse(
            "# rules\n\nadd_zero: Add(x, 0) # pattern\n"
            "    # replacement\n    => x\n\n"
            "sub_self: Sub(x, x)\n  => 0\n"
        )
        self.assertEqual([rule.name for rule in rules], ["add_zero", "sub_self"])
        self.assertEqual(rules[1].replacement.value, 0)

    def test_nested_patterns_and_calculation(self):
        rule = parse(
            "add_reassoc: Add(Add(x, C1), C2)\n"
            "    C = C1 + C2\n    => Add(x, C)\n"
        )[0]
        self.assertEqual(rule.pattern.args[0].name, "Add")
        self.assertEqual(rule.body[0].name, "C")
        self.assertIsInstance(rule.body[0].value, ast.Binary)
        self.assertEqual(rule.replacement.args[1].name, "C")

    def test_optional_type_annotations(self):
        rule = parse(
            "resize_roundtrip: ResizeU(ResizeU(x: T): U): T\n"
            "    where width(U) >= width(T)\n    => x\n"
        )[0]
        self.assertIsInstance(rule.pattern, ast.Typed)
        self.assertEqual(rule.pattern.type_name, "T")
        inner = rule.pattern.value.args[0]
        self.assertEqual(inner.type_name, "U")
        self.assertEqual(inner.value.args[0].type_name, "T")
        self.assertEqual(rule.body[0].kind, "where")
        self.assertEqual(rule.body[0].value.operators, (">=",))

    def test_annotated_replacement_and_constants(self):
        rule = parse("zero: Sub(x: Int32, x)\n    => -0xff: Int32\n")[0]
        self.assertEqual(rule.replacement.type_name, "Int32")
        self.assertEqual(rule.replacement.value.value, -255)

    def test_arithmetic_precedence(self):
        value = self.calculation("C1 + C2 * 3 << 1 & 0xff ^ 2 | 4")
        self.assertEqual(value.operator, "|")
        self.assertEqual(value.left.operator, "^")
        self.assertEqual(value.left.left.operator, "&")
        shift = value.left.left.left
        self.assertEqual(shift.operator, "<<")
        self.assertEqual(shift.left.operator, "+")
        self.assertEqual(shift.left.right.operator, "*")

    def test_parentheses_unary_and_left_associativity(self):
        value = self.calculation("~(C1 + C2) - -C3 - +C4")
        self.assertEqual(value.operator, "-")
        self.assertEqual(value.left.operator, "-")
        self.assertEqual(value.left.left.operator, "~")
        self.assertEqual(value.left.left.operand.operator, "+")
        self.assertEqual(value.left.right.operator, "-")
        self.assertEqual(value.right.operator, "+")

    def test_boolean_precedence_and_chained_comparisons(self):
        rule = parse(
            "shift: Shl(x, C)\n"
            "    check not C == 0 or 0 <= C < width(T) and C & 1 == 0\n"
            "    => x\n"
        )[0]
        value = rule.body[0].value
        self.assertEqual(value.operator, "or")
        self.assertEqual(value.left.operator, "not")
        self.assertEqual(value.left.operand.operators, ("==",))
        self.assertEqual(value.right.operator, "and")
        self.assertEqual(value.right.left.operators, ("<=", "<"))
        self.assertEqual(value.right.right.operands[0].operator, "&")

    def test_calls_and_literals(self):
        value = self.calculation("max(width(T), 0b10, 0x10, 0010) // 2 % 3")
        self.assertEqual(value.operator, "%")
        self.assertEqual(value.left.operator, "//")
        call = value.left.left
        self.assertEqual(call.name, "max")
        self.assertEqual([arg.value for arg in call.args[1:]], [2, 16, 10])

    def test_spans(self):
        source = "add: Add(x: T, C)\n    R = C + 1\n    => Add(x, R): T\n"
        rule = parse(source)[0]
        nodes = [rule.pattern, rule.body[0], rule.replacement]
        self.assertEqual([source[node.span.start:node.span.end] for node in nodes],
                         ["Add(x: T, C)", "R = C + 1", "Add(x, R): T"])
        self.assertEqual((rule.body[0].span.line, rule.body[0].span.column), (2, 5))

    def test_empty_file(self):
        self.assertEqual(parse(""), ())
        self.assertEqual(parse("# empty\n\n"), ())

    def test_reject_invalid_rules(self):
        cases = [
            ("a: x\n    => x", "rule pattern must be an operation"),
            ("a: Add(x, 0)\n=> x", "expected 'INDENT'"),
            ("a: Add(x, 0)\n    C = 1", "expected replacement"),
            ("a: Add(x, 0)\n    =>", "expected value"),
            ("a: Add(x, 0)\n    => x\n    => x", "expected 'DEDENT'"),
            ("a: Add(x, 0)\n    C =\n    => x", "expected expression"),
            ("a: Add(x, 0)\n    check\n    => x", "expected expression"),
            ("a: Add(x,)\n    => x", "expected argument"),
            ("a: Add(x, 0)\n    => x + 1", "expected 'NEWLINE'"),
            ("a: Add(x, 0)\n    C = 1 / 2\n    => x", "expected 'NEWLINE'"),
            ("a: Add(x, 0)\n    C = (1 +\n        2)\n    => x", "expected expression"),
            ("a: Add(x, 0)\n    check 1\n        => x", "expected calculation"),
            ("a: Add(x: , 0)\n    => x", "expected 'NAME'"),
        ]
        for source, message in cases:
            with self.subTest(source=source):
                with self.assertRaises(ParseError) as raised:
                    parse(source, "bad.rules")
                self.assertIn(message, str(raised.exception))


if __name__ == "__main__":
    unittest.main()
