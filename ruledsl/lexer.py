import re
from dataclasses import dataclass

from .source import ParseError, Span


TOKEN_RE = re.compile(
    r"(?P<SPACE>[ \t]+)"
    r"|(?P<NUMBER>0[xX][0-9a-fA-F]+|0[bB][01]+|[0-9]+)"
    r"|(?P<NAME>[A-Za-z_][A-Za-z_0-9]*)"
    r"|(?P<OP>=>|<<|>>|<=|>=|==|!=|//|[():,=+\-*/%&|^~<>])"
    r"|(?P<COMMENT>\#.*)"
)
KEYWORDS = {"check", "where", "and", "or", "not"}


@dataclass(frozen=True)
class Token:
    kind: str
    text: str
    span: Span


def tokenize(source, filename="<string>"):
    tokens = []
    indents = [0]
    offset = 0
    lines = source.split("\n")
    for line_number, raw_line in enumerate(lines, 1):
        has_newline = line_number < len(lines)
        line = raw_line.removesuffix("\r") if has_newline else raw_line
        leading = len(line) - len(line.lstrip(" \t"))
        if not line[leading:] or line[leading:].startswith("#"):
            offset += len(raw_line) + int(has_newline)
            continue
        if "\t" in line[:leading]:
            position = line.index("\t")
            raise ParseError("use spaces for indentation", Span(
                offset + position, offset + position + 1, line_number, position + 1
            ), source, filename)
        span = Span(offset + leading, offset + leading, line_number, leading + 1)
        if leading > indents[-1]:
            indents.append(leading)
            tokens.append(Token("INDENT", "", span))
        elif leading < indents[-1]:
            while leading < indents[-1]:
                indents.pop()
                tokens.append(Token("DEDENT", "", span))
            if leading != indents[-1]:
                raise ParseError("inconsistent indentation", span, source, filename)
        position = leading
        while position < len(line):
            match = TOKEN_RE.match(line, position)
            if match is None:
                raise ParseError(f"unexpected character {line[position]!r}", Span(
                    offset + position, offset + position + 1, line_number, position + 1
                ), source, filename)
            kind = match.lastgroup
            text = match.group()
            if kind == "COMMENT":
                break
            if kind != "SPACE":
                if kind == "OP" or kind == "NAME" and text in KEYWORDS:
                    kind = text
                tokens.append(Token(kind, text, Span(
                    offset + position, offset + match.end(), line_number, position + 1
                )))
            position = match.end()
        tokens.append(Token("NEWLINE", "", Span(
            offset + len(line), offset + len(raw_line) + int(has_newline),
            line_number, len(line) + 1
        )))
        offset += len(raw_line) + int(has_newline)
    end = Span(len(source), len(source), len(lines), len(lines[-1]) + 1)
    for indent in indents[1:]:
        tokens.append(Token("DEDENT", "", end))
    tokens.append(Token("EOF", "", end))
    return tokens
