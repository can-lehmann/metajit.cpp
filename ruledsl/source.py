from dataclasses import dataclass


@dataclass(frozen=True)
class Span:
    start: int
    end: int
    line: int
    column: int


class SourceError(Exception):
    def __init__(self, message, span, source, filename):
        self.message = message
        self.span = span
        self.filename = filename
        line = source.split("\n")[span.line - 1].rstrip("\r")
        super().__init__(
            f"{filename}:{span.line}:{span.column}: {message}\n"
            f"{line}\n{' ' * (span.column - 1)}^"
        )


class ParseError(SourceError):
    pass


class TypeCheckError(SourceError):
    pass
