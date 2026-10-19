from . import ast
from .lexer import tokenize
from .source import ParseError, Span


PRECEDENCE = {
    "or": 1, "and": 2,
    "==": 3, "!=": 3, "<": 3, "<=": 3, ">": 3, ">=": 3,
    "|": 4, "^": 5, "&": 6,
    "<<": 7, ">>": 7,
    "+": 8, "-": 8,
    "*": 9, "//": 9, "%": 9,
}
COMPARISONS = {"==", "!=", "<", "<=", ">", ">="}


class Parser:
    def __init__(self, source, filename):
        self.source = source
        self.filename = filename
        self.tokens = tokenize(source, filename)
        self.position = 0

    @property
    def current(self):
        return self.tokens[self.position]

    def accept(self, kind):
        if self.current.kind == kind:
            token = self.current
            self.position += 1
            return token
        return None

    def expect(self, kind):
        token = self.accept(kind)
        if token is None:
            self.error(f"expected {kind!r}, got {self.current.kind!r}")
        return token

    def error(self, message):
        raise ParseError(message, self.current.span, self.source, self.filename)

    def span_from(self, start):
        return Span(start.start, self.tokens[self.position - 1].span.end,
                    start.line, start.column)

    def parse(self):
        rules = []
        while self.current.kind != "EOF":
            rules.append(self.rule())
        return tuple(rules)

    def rule(self):
        name = self.expect("NAME")
        self.expect(":")
        pattern = self.pattern()
        root = pattern.value if isinstance(pattern, ast.Typed) else pattern
        if not isinstance(root, ast.Call):
            raise ParseError("rule pattern must be an operation", pattern.span,
                             self.source, self.filename)
        self.expect("NEWLINE")
        self.expect("INDENT")
        small_widths_only = self.accept("SORRY_Z3_SMALL_WIDTHS") is not None
        if small_widths_only:
            self.expect("NEWLINE")
        body = []
        while self.current.kind != "=>":
            start = self.current.span
            if self.current.kind in {"check", "where"}:
                kind = self.current.kind
                self.position += 1
                value = self.expression()
                body.append(ast.Condition(self.span_from(start), kind, value))
            elif self.current.kind == "NAME":
                binding = self.expect("NAME")
                self.expect("=")
                value = self.expression()
                body.append(ast.Binding(self.span_from(start), binding.text, value))
            elif self.current.kind in {"DEDENT", "EOF"}:
                self.error("expected replacement after '=>'")
            else:
                self.error("expected calculation, 'check', 'where', or '=>'")
            self.expect("NEWLINE")
        self.expect("=>")
        replacement = self.pattern()
        self.expect("NEWLINE")
        end = self.tokens[self.position - 2].span.end
        self.expect("DEDENT")
        return ast.Rule(Span(name.span.start, end, name.span.line, name.span.column),
                        name.text, pattern, tuple(body), replacement, small_widths_only)

    def annotation(self, value):
        if self.accept(":"):
            type_name = self.expect("NAME")
            return ast.Typed(self.span_from(value.span), value, type_name.text)
        return value

    def number(self):
        token = self.expect("NUMBER")
        base = 16 if token.text.lower().startswith("0x") else (
            2 if token.text.lower().startswith("0b") else 10
        )
        return ast.Number(token.span, int(token.text, base))

    def pattern(self):
        start = self.current.span
        if self.current.kind in {"+", "-"}:
            operator = self.current.kind
            self.position += 1
            number = self.number()
            value = ast.Number(self.span_from(start),
                               -number.value if operator == "-" else number.value)
        elif self.current.kind == "NUMBER":
            value = self.number()
        elif self.accept("("):
            value = self.pattern()
            self.expect(")")
        elif self.current.kind == "NAME":
            name = self.expect("NAME")
            if self.accept("("):
                args = self.arguments(pattern=True)
                value = ast.Call(self.span_from(start), name.text, args)
            else:
                value = ast.Name(name.span, name.text)
        else:
            self.error("expected value, constant, or operation pattern")
        return self.annotation(value)

    def arguments(self, pattern=False):
        args = []
        if self.accept(")"):
            return tuple(args)
        while True:
            args.append(self.pattern() if pattern else self.expression())
            if self.accept(")"):
                return tuple(args)
            self.expect(",")
            if self.current.kind == ")":
                self.error("expected argument after ','")

    def expression(self, minimum=1):
        start = self.current.span
        if self.current.kind in {"+", "-", "~", "not"}:
            operator = self.current.kind
            self.position += 1
            operand = self.expression(3 if operator == "not" else 10)
            left = ast.Unary(self.span_from(start), operator, operand)
        elif self.accept("("):
            left = self.expression()
            self.expect(")")
        elif self.current.kind == "NUMBER":
            left = self.number()
        elif self.current.kind == "NAME":
            name = self.expect("NAME")
            if self.accept("("):
                args = self.arguments()
                left = ast.Call(self.span_from(start), name.text, args)
            else:
                left = ast.Name(name.span, name.text)
        else:
            self.error("expected expression")
        while PRECEDENCE.get(self.current.kind, 0) >= minimum:
            operator = self.current.kind
            precedence = PRECEDENCE[operator]
            if operator in COMPARISONS:
                operands = [left]
                operators = []
                while self.current.kind in COMPARISONS:
                    operators.append(self.current.kind)
                    self.position += 1
                    operands.append(self.expression(precedence + 1))
                left = ast.Compare(self.span_from(start), tuple(operands), tuple(operators))
            else:
                self.position += 1
                right = self.expression(precedence + 1)
                left = ast.Binary(self.span_from(start), operator, left, right)
        return left


def parse(source, filename="<string>"):
    return Parser(source, filename).parse()
