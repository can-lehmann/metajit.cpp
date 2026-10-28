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
from itertools import product


@dataclass(frozen=True)
class ConcreteType:
    name: str
    width: int


BOOL = ConcreteType("Bool", 1)
INT8 = ConcreteType("Int8", 8)
INT16 = ConcreteType("Int16", 16)
INT32 = ConcreteType("Int32", 32)
INT64 = ConcreteType("Int64", 64)
INT_TYPES = (INT8, INT16, INT32, INT64)
BIT_TYPES = (BOOL, *INT_TYPES)
CONCRETE_TYPES = {type_.name: type_ for type_ in BIT_TYPES}


@dataclass(frozen=True)
class TypeVar:
    index: int


class TypeConflict(Exception):
    pass


class TypeConstraints:
    def __init__(self):
        # parents forms a union find of type variables
        self.parents: list[int] = []
        # domains store the sets of concrete types a type variable can have on
        # the root (None otherwise)
        self.domains: list[set[ConcreteType] | None] = []

    def fresh(self, domain=BIT_TYPES):
        variable = TypeVar(len(self.parents))
        self.parents.append(variable.index)
        self.domains.append(set(domain))
        return variable

    def resolve(self, variable):
        index = variable.index
        while self.parents[index] != index:
            self.parents[index] = self.parents[self.parents[index]]
            index = self.parents[index]
        return TypeVar(index)

    def restrict(self, variable, domain):
        variable = self.resolve(variable)
        intersection = self.domains[variable.index].intersection(domain)
        if not intersection:
            expected = ", ".join(type_.name for type_ in BIT_TYPES if type_ in domain)
            actual = ", ".join(type_.name for type_ in BIT_TYPES
                               if type_ in self.domains[variable.index])
            raise TypeConflict(f"incompatible types: {actual}; expected {expected}")
        self.domains[variable.index] = intersection

    def unify(self, left, right):
        left = self.resolve(left)
        right = self.resolve(right)
        if left == right:
            return
        self.restrict(left, self.domains[right.index])
        self.parents[right.index] = left.index
        self.domains[right.index] = None

    def assignments(self):
        roots = [TypeVar(index) for index, parent in enumerate(self.parents)
                 if parent == index]
        domains = [tuple(type_ for type_ in BIT_TYPES
                         if type_ in self.domains[root.index]) for root in roots]
        for concrete in product(*domains):
            roots_assignment = dict(zip(roots, concrete))
            yield {TypeVar(index): roots_assignment[self.resolve(TypeVar(index))]
                   for index in range(len(self.parents))}
