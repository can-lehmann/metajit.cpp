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

from .types import BIT_TYPES, BOOL, ConcreteType, INT_TYPES


@dataclass(frozen=True)
class Signature:
    parameters: tuple[str, ...]
    result: str
    domains: tuple[tuple[str, tuple[ConcreteType, ...]], ...]
    commutative: bool = False
    unrestricted_types: tuple[str, ...] = ()


SIGNATURES = {}

for name in ("Add", "Sub", "Mul", "MulHighU", "DivS", "DivU", "ModS", "ModU", "Shl", "ShrU", "ShrS"):
    SIGNATURES[name] = Signature(("T", "T"), "T", (("T", INT_TYPES),),
                                 name in {"Add", "Mul", "MulHighU"})

for name in ("And", "Or", "Xor"):
    SIGNATURES[name] = Signature(("T", "T"), "T", (("T", BIT_TYPES),), True)

SIGNATURES["Eq"] = Signature(("T", "T"), "B", (("T", BIT_TYPES), ("B", (BOOL,))), True, ("T",))

for name in ("LtU", "LtS"):
    SIGNATURES[name] = Signature(("T", "T"), "B", (("T", INT_TYPES), ("B", (BOOL,))))

SIGNATURES["Select"] = Signature(("B", "T", "T"), "T", (("B", (BOOL,)), ("T", BIT_TYPES)),
                                unrestricted_types=("T",))

for name in ("ResizeU", "ResizeS", "ResizeX"):
    SIGNATURES[name] = Signature(("S",), "T", (("S", BIT_TYPES), ("T", BIT_TYPES)))

for name in ("Popcount", "BitReverse"):
    SIGNATURES[name] = Signature(("T",), "T", (("T", INT_TYPES),))
SIGNATURES["Freeze"] = Signature(("T",), "T", (("T", BIT_TYPES),), unrestricted_types=("T",))
