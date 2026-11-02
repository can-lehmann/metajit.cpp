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

from pathlib import Path
import shutil
import unittest

import z3

from . import ast, test_simplify
from .generate import generate
from .lower import lower as lower_builder
from .lower_simplify import lower
from .matcher import build_matchers
from .parser import parse
from .proof import ProofError, Prover, prove
from .source import ParseError, TypeCheckError
from .typecheck import check
from .types import BIT_TYPES


RULES = (Path(__file__).resolve().parent.parent / "usedbits.rules").read_text()


class UsedBitsTests(unittest.TestCase):
    def test_parse_and_format(self):
        source = "example: And(Or(x, C1) @single_use, C2): T @used_bits(Cused)\n    => x\n"
        rule, = parse(source)
        self.assertEqual(rule.pattern.value.used_bits, "Cused")
        self.assertTrue(rule.pattern.value.args[0].single_use)
        self.assertEqual(ast.format_pattern(rule.pattern), source.split(": ", 1)[1].split("\n")[0])
        nested_only, = parse("example: And(Or(x, C1) @single_use, C2): T\n    => x\n")
        self.assertEqual(ast.format_pattern(nested_only.pattern),
                         "And(Or(x, C1) @single_use, C2): T")
        for annotation in ("@used_bits()", "@used_bits(C, D)", "@used_bits(C) @used_bits(D)"):
            with self.subTest(annotation=annotation), self.assertRaises(ParseError):
                parse(f"bad: And(x, C) {annotation}\n    => x\n")

    def test_mask_type_is_root_type(self):
        checked, = check("resize: ResizeU(x: T): U @used_bits(Cused)\n    => Cused\n", used_bits=True)
        self.assertEqual(checked.used_bits_binding, "Cused")
        for assignment in checked.instantiations:
            self.assertEqual(assignment[checked.bindings["Cused"].type],
                             checked.type_of(checked.rule.pattern, assignment))
        self.assertEqual(len(checked.instantiations), len(BIT_TYPES) ** 2)

    def test_annotation_validation(self):
        cases = [
            ("bad: And(x, C)\n    => x\n", "require a root"),
            ("bad: And(x, C) @used_bits(C)\n    => x\n", "already|duplicate"),
            ("bad: And(x, C) @used_bits(x)\n    => x\n", "start with C"),
            ("bad: And(Or(x, C1) @used_bits(Cmask), C2) @used_bits(Cused)\n    => x\n", "root input"),
            ("bad: And(x, C) @used_bits(Cused)\n    => And(x, C) @used_bits(Cother)\n", "root input"),
            ("bad: And(x, C) @used_bits(Cused)\n    Cused = 1\n    => x\n", "duplicate"),
            ("bad: And(x, C) @used_bits(Cused)\n    where Cused == 0\n    => x\n", "use check"),
        ]
        for source, message in cases:
            with self.subTest(source=source), self.assertRaisesRegex(TypeCheckError, message):
                check(source, used_bits=True)

    def test_known_bits_queries_rejected(self):
        for function in ("known_zeros", "known_ones"):
            for body in (f"Cunused = {function}(x)", f"check {function}(x) == 0"):
                source = f"bad: And(x, C) @used_bits(Cused)\n    {body}\n    => x\n"
                with self.subTest(body=body), self.assertRaisesRegex(TypeCheckError, "KnownBits queries"):
                    check(source, used_bits=True)

    def test_full_value_sources_reject_annotation(self):
        source = "masked: And(x, C) @used_bits(Cused)\n    => x\n"
        for role in ("fold", "simplify"):
            with self.subTest(role=role), self.assertRaisesRegex(TypeCheckError, "masked proof contract"):
                if role == "fold":
                    generate(source)
                else:
                    generate("", include_simplify=True, simplify_source=source)
        with self.assertRaisesRegex(ValueError, "requires include_simplify"):
            generate("", used_bits_source=source)
        with self.assertRaisesRegex(TypeCheckError, "require a root"):
            generate("", include_simplify=True, used_bits_source="plain: And(x, C)\n    => x\n")

    def test_rules_prove(self):
        rules = prove(RULES, used_bits=True)
        self.assertEqual([len(rule.instantiations) for rule in rules], [5, 5, 5, 10, 10, 5, 5])
        for checked in rules:
            for assignment in checked.instantiations:
                prover = Prover(checked, assignment, RULES, "usedbits.rules", 10000)
                prover.prove()
                self.assertFalse(prover.known_bits)
                self.assertEqual(prover.solver.check(prover.used_mask == 0), z3.sat)
                self.assertEqual(prover.solver.check(prover.used_mask == -1), z3.sat)

    def test_observed_bits_counterexample(self):
        source = "wrong: And(x, C) @used_bits(Cused)\n    => x\n"
        with self.assertRaises(ProofError) as raised:
            prove(source, used_bits=True)
        example = raised.exception.counterexample
        mask = example.inputs["Cused"]
        self.assertFalse(mask.poison)
        self.assertNotEqual((example.before.value ^ example.after.value) & mask.value, 0)

    def test_zero_mask_still_preserves_definedness(self):
        for pattern in ("Freeze(x)", "Select(c, x, y)"):
            source = f"wrong: {pattern} @used_bits(Cused)\n    check Cused == 0\n    => x\n"
            with self.subTest(pattern=pattern), self.assertRaises(ProofError) as raised:
                prove(source, used_bits=True)
            example = raised.exception.counterexample
            self.assertEqual(example.inputs["Cused"].value, 0)
            self.assertFalse(example.before.poison)
            self.assertTrue(example.after.poison)
        prove("zero: And(x, C) @used_bits(Cused)\n    check Cused == 0\n    => x\n", used_bits=True)

    def test_all_ones_mask_requires_full_equality(self):
        with self.assertRaises(ProofError):
            prove("wrong: And(x, C) @used_bits(Cused)\n    check Cused == -1\n    => x\n", used_bits=True)

    def test_masked_matchers_cannot_use_full_value_backend(self):
        checked = check("same: And(x, C) @used_bits(Cused)\n    => x\n", used_bits=True)
        program = build_matchers(checked)
        for lowerer in (lower_builder, lower):
            with self.subTest(lowerer=lowerer), self.assertRaisesRegex(ValueError, "masked|separate"):
                lowerer(program)
        plain = build_matchers(check("same: And(x, C)\n    => x\n"))
        with self.assertRaisesRegex(ValueError, "separate"):
            lower(plain, used_bits=True)


class UsedBitsEmissionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = shutil.which("clang++") or shutil.which("g++")

    def test_entry_points_and_mask_materialization(self):
        folds = "fold_id: Or(x, 0)\n    => x\n"
        full = "full_id: And(x, x)\n    => x\n"
        extra = "mask_value: Xor(x, 0) @used_bits(Cused)\n    check Cused == 0\n    => Cused\n"
        code = generate(folds, include_simplify=True, simplify_source=full,
                        used_bits_source=RULES + extra)
        main = """
int main() {
  Generated builder;
  for (Type type : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Value x(type);
    uint64_t mask = type_mask(type);
    Const one(type, 1), zero(type, 0), five(type, 5);
    for (bool swapped : {false, true}) {
      Inst* inner = builder.build_and(&x, &one);
      Inst* root = swapped ? builder.build_and(&one, &x) : builder.build_and(&x, &one);
      size_t before = builder.allocated.size();
      assert(!builder.try_simplify(root));
      if (type != Type::Bool) {
        assert(!builder.try_simplify_used_bits(root, mask));
      }
      assert(builder.allocated.size() == before);
      assert(builder.try_simplify_used_bits(root, 1) == &x);
      assert(builder.try_simplify_used_bits(root, 0) == &x);
      assert(builder.allocated.size() == before);
      root = builder.build_or(&x, &one);
      if (type != Type::Bool) {
        assert(builder.try_simplify_used_bits(root, 2) == &x);
        root = builder.build_and(&x, &five);
        Value* replacement = builder.try_simplify_used_bits(root, 3);
        AndInst* result = dynamic_cast<AndInst*>(replacement);
        Const* bits = result ? dynamic_cast<Const*>(result->arg(1)) : nullptr;
        assert(result && result->arg(0) == &x && bits && bits->value() == 1);
      }
      root = builder.build_xor(&x, &zero);
      Const* materialized = dynamic_cast<Const*>(builder.try_simplify_used_bits(root, 0));
      assert(materialized && materialized->type() == type && materialized->value() == 0);
      assert(builder.try_simplify(builder.build_and(inner, inner)) == inner);
    }
  }
  for (Type source : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    for (Type target : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
      if (type_width(target) <= type_width(source)) { continue; }
      Value narrow(source);
      for (bool signed_ : {false, true}) {
        Inst* resize = signed_ ? static_cast<Inst*>(builder.build_resize_s(&narrow, target))
                              : builder.build_resize_u(&narrow, target);
        size_t before = builder.allocated.size();
        assert(!builder.try_simplify_used_bits(resize, type_mask(target)));
        assert(builder.allocated.size() == before);
        ResizeXInst* result = dynamic_cast<ResizeXInst*>(builder.try_simplify_used_bits(resize, type_mask(source)));
        assert(result && result->arg(0) == &narrow && result->type() == target);
      }
    }
  }
  assert(Generated::ruledsl_rule_counts[1] > 0);
  assert(Generated::ruledsl_rule_counts[2] > 0);
  assert(Generated::ruledsl_rule_counts[9] > 0);
}
"""
        test_simplify.SimplifyTests.run_cpp(self, code, main, statistics=True)

    def test_xor_elimination_and_or_mask_shrinking(self):
        code = generate("", include_simplify=True, used_bits_source=RULES)
        main = """
int main() {
  Generated builder;
  for (Type type : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Value x(type);
    uint64_t mask = type_mask(type);
    uint64_t top = uint64_t(1) << (type_width(type) - 1);
    for (uint64_t bits : {uint64_t(0), uint64_t(1), uint64_t(5), top, mask}) {
      Const constant(type, bits);
      for (uint64_t demand : {uint64_t(0), uint64_t(1), uint64_t(3), top, mask}) {
        uint64_t used = demand & mask;
        for (bool swapped : {false, true}) {
          Inst* root = swapped ? builder.build_xor(&constant, &x) : builder.build_xor(&x, &constant);
          size_t before = builder.allocated.size();
          assert(!builder.try_simplify(root));
          Value* replacement = builder.try_simplify_used_bits(root, demand);
          assert(replacement == ((constant.value() & used) == 0 ? &x : nullptr));
          assert(builder.allocated.size() == before);
          root = swapped ? builder.build_or(&constant, &x) : builder.build_or(&x, &constant);
          before = builder.allocated.size();
          assert(!builder.try_simplify(root));
          replacement = builder.try_simplify_used_bits(root, demand);
          uint64_t narrowed = constant.value() & used;
          if (narrowed == 0) {
            assert(replacement == &x);
            assert(builder.allocated.size() == before);
          } else if (narrowed == constant.value()) {
            assert(!replacement);
            assert(builder.allocated.size() == before);
          } else {
            OrInst* result = dynamic_cast<OrInst*>(replacement);
            Const* result_bits = result ? dynamic_cast<Const*>(result->arg(1)) : nullptr;
            assert(result && result->arg(0) == &x);
            assert(result_bits && result_bits->value() == narrowed);
          }
        }
      }
    }
  }
}
"""
        test_simplify.SimplifyTests.run_cpp(self, code, main)

    def test_subtree_reuse_and_single_use_guards(self):
        source = ("reuse: Or(Or(x, C) @single_use, C) @used_bits(Cused)\n"
                  "    => Or(x, C)\n")
        code = generate("", include_simplify=True, used_bits_source=source)
        main = """
struct Counts {
  Value* value;
  uint8_t count;
  uint8_t at(Value* arg) const { return arg == value ? count : 255; }
};
int main() {
  Generated builder;
  Value x(Type::Int32);
  Const one(Type::Int32, 1);
  Inst* inner = builder.build_or(&x, &one);
  Inst* root = builder.build_or(inner, &one);
  size_t before = builder.allocated.size();
  assert(!builder.try_simplify(root));
  assert(!builder.try_simplify_used_bits(root, 0xffffffff));
  assert(!builder.try_simplify_used_bits(root, 0xffffffff, Counts{inner, 2}));
  assert(builder.try_simplify_used_bits(root, 0xffffffff, Counts{inner, 1}) == inner);
  assert(builder.allocated.size() == before);
}
"""
        test_simplify.SimplifyTests.run_cpp(self, code, main)

    def test_disabled_backend_returns_no_change(self):
        code = "#define METAJIT_DISABLE_RULEDSL\n" + generate("", include_simplify=True, used_bits_source=RULES)
        main = """
int main() {
  Generated builder;
  Value x(Type::Int32);
  Const one(Type::Int32, 1);
  Inst* root = builder.build_and(&x, &one);
  size_t before = builder.allocated.size();
  assert(!builder.try_simplify_used_bits(root, 0));
  assert(builder.allocated.size() == before);
}
"""
        test_simplify.SimplifyTests.run_cpp(self, code, main)
