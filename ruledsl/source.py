# Copyright 2026 Can Joshua Lehmann
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

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
