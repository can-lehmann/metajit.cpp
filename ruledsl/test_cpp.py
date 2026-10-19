import contextlib
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from .__main__ import main
from .generate import generate
from .proof import ProofError


RULES = """add_zero: Add(x, 0)
    => x
add_constants: Add(C1, C2)
    C = C1 + C2
    => C
add_reassoc: Add(Add(x, C1), C2)
    C = C1 + C2
    => Add(x, C)
sub_self: Sub(x, x)
    => 0
sub_constant: Sub(x, C)
    class = -C
    ruledsl_rule_counts = class
    => Add(x, ruledsl_rule_counts)
mul_two: Mul(x, 2)
    => Shl(x, 1)
mul_power_of_two: Mul(x, C)
    check C != 0 and C & (C - 1) == 0
    Cshift = ctz(C)
    => Shl(x, Cshift)
and_xor_constant: And(Xor(x, C), x)
    Cmask = ~C
    => And(x, Cmask)
and_xor_subset_zero: And(Xor(And(x, y), x), And(x, y))
    => 0
and_absorb: And(x, And(x, y))
    => And(x, y)
and_common_operand: And(And(x, y), And(x, z))
    => And(And(x, y), z)
and_all_ones: And(x, -1)
    => x
or_zero: Or(x, 0)
    => x
or_common_operand: Or(Or(x, y), Or(x, z))
    => Or(Or(x, y), z)
or_reassoc_constants: Or(Or(x, C1), C2)
    C = C1 | C2
    => Or(x, C)
xor_self: Xor(x, x)
    => 0
xor_reassoc_constants: Xor(Xor(x, C1), C2)
    C = C1 ^ C2
    => Xor(x, C)
xor_cancel: Xor(Xor(x, y), y)
    => x
xor_common_operand: Xor(Xor(x, y), Xor(x, z))
    => Xor(y, z)
div_constant: DivU(C1, C2)
    check C2 != 0 and C1 // C2 <= C1
    C = C1 // C2
    => C
div_one: DivS(x, 1)
    => x
mod_one: ModS(x, 1)
    => 0
mod_u_one: ModU(x, 1)
    => 0
shift_constant: Shl(C1: T, C2)
    check 0 <= C2 < width(T)
    C = C1 << C2
    => C
shr_zero: ShrU(x, 0)
    => x
shrs_zero: ShrS(x, 0)
    => x
eq_self: Eq(x, x)
    => 1
eq_constants: Eq(C1, C2)
    C = C1 == C2
    => C
lt_u_constants: LtU(C1, C2)
    C = C1 < C2
    => C
select_comparison: Select(Eq(C1, C2), x, y)
    different = C1 != C2
    same = not different
    => Select(same, x, y)
lt_self: LtU(x, x)
    => 0
lts_self: LtS(x, x)
    => 0
select_same: Select(cond, x, x)
    => x
resize_roundtrip: ResizeU(ResizeU(x: T): U): T
    where width(U) >= width(T)
    => x
resize_constant: ResizeU(C: T): U
    C2 = bits(C, U)
    => C2
resize_s_same: ResizeS(x: T): T
    quotient = 1 // (width(T) - 1)
    n = width(T)
    where n > 1
    check quotient == 0
    => x
resize_x_same: ResizeX(x: T): T
    => x
resize_x_sign_bit: ResizeX(ShrU(x: T, C)): Bool
    check C == width(T) - 1
    => LtS(x, 0)
popcount_zero: Popcount(And(x, 0))
    => 0
freeze_constant: Freeze(C: T)
    count = ctz(C)
    check C == 0 and count == width(T) or C != 0 and count < width(T)
    => C
"""


MAIN = """
int main() {
  Generated builder;
  Value x(Type::Int8), y(Type::Int8);
  Const zero(Type::Int8, 0), one(Type::Int8, 1), two(Type::Int8, 2);
  Const three(Type::Int8, 3), eight(Type::Int8, 8), nine(Type::Int8, 9);
  Const ten(Type::Int8, 10), large(Type::Int8, 250);
  unsigned builds = builder.builds;
  assert(builder.fold_add(&x, &zero) == &x);
  assert(builder.fold_add(&zero, &x) == &x);
  assert(builder.builds == builds);
  assert(dynamic_cast<AddInst*>(builder.fold_add(&x, &y)));
  assert(builder.handwritten_calls == 1);
  for (unsigned inner_order = 0; inner_order < 2; ++inner_order) {
    Value* inner = builder.build_add(inner_order ? &large : &x, inner_order ? &x : &large);
    for (unsigned outer_order = 0; outer_order < 2; ++outer_order) {
      Value* result = builder.fold_add(outer_order ? &ten : inner, outer_order ? inner : &ten);
      AddInst* add = dynamic_cast<AddInst*>(result);
      assert(add && add->arg(0) == &x);
      assert(dynamic_cast<Const*>(add->arg(1))->value() == 4);
    }
  }
  Type types[] = {Type::Int8, Type::Int16, Type::Int32, Type::Int64};
  Type bit_types[] = {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64};
  for (Type type: bit_types) {
    Value a(type), b(type), c(type);
    Const freeze_zero(type, 0);
    assert(builder.fold_freeze(&freeze_zero) == &freeze_zero);
    for (unsigned order = 0; order < 16; ++order) {
      Value* subset = builder.build_and(order & 1 ? &b : &a, order & 1 ? &a : &b);
      Value* xored = builder.build_xor(order & 2 ? subset : &a, order & 2 ? &a : subset);
      Value* other = builder.build_and(order & 8 ? &b : &a, order & 8 ? &a : &b);
      Const* result = dynamic_cast<Const*>(builder.fold_and(order & 4 ? xored : other,
                                                          order & 4 ? other : xored));
      assert(result && result->type() == type && result->value() == 0);
    }
    Const mask(type, 5);
    for (unsigned order = 0; order < 4; ++order) {
      Value* xored = builder.build_xor(order & 1 ? &mask : &a, order & 1 ? &a : &mask);
      AndInst* result = dynamic_cast<AndInst*>(builder.fold_and(order & 2 ? xored : &a,
                                                              order & 2 ? &a : xored));
      assert(result && result->arg(0) == &a);
      assert(dynamic_cast<Const*>(result->arg(1))->value() == (type_mask(type) ^ mask.value()));
    }
    for (unsigned order = 0; order < 8; ++order) {
      Value* left_a = order & 1 ? &b : &a;
      Value* left_b = order & 1 ? &a : &b;
      Value* right_a = order & 2 ? &c : &a;
      Value* right_b = order & 2 ? &a : &c;
      AndInst* left_and = builder.build_and(left_a, left_b);
      AndInst* right_and = builder.build_and(right_a, right_b);
      OrInst* left_or = builder.build_or(left_a, left_b);
      OrInst* right_or = builder.build_or(right_a, right_b);
      XorInst* left_xor = builder.build_xor(left_a, left_b);
      XorInst* right_xor = builder.build_xor(right_a, right_b);
      builds = builder.builds;
      AndInst* result_and = dynamic_cast<AndInst*>(builder.fold_and(order & 4 ? right_and : left_and,
                                                                   order & 4 ? left_and : right_and));
      assert(builder.builds == builds + 1);
      assert(result_and && ((result_and->arg(0) == left_and && result_and->arg(1) == &c)
                           || (result_and->arg(0) == right_and && result_and->arg(1) == &b)));
      builds = builder.builds;
      OrInst* result_or = dynamic_cast<OrInst*>(builder.fold_or(order & 4 ? right_or : left_or,
                                                              order & 4 ? left_or : right_or));
      assert(builder.builds == builds + 1);
      assert(result_or && ((result_or->arg(0) == left_or && result_or->arg(1) == &c)
                          || (result_or->arg(0) == right_or && result_or->arg(1) == &b)));
      builds = builder.builds;
      XorInst* result_xor = dynamic_cast<XorInst*>(builder.fold_xor(order & 4 ? right_xor : left_xor,
                                                                 order & 4 ? left_xor : right_xor));
      assert(builder.builds == builds + 1);
      assert(result_xor && ((result_xor->arg(0) == &b && result_xor->arg(1) == &c)
                           || (result_xor->arg(0) == &c && result_xor->arg(1) == &b)));
    }
    for (unsigned inner_order = 0; inner_order < 2; ++inner_order) {
      Value* inner = builder.build_xor(inner_order ? &a : &b, inner_order ? &b : &a);
      for (unsigned outer_order = 0; outer_order < 2; ++outer_order) {
        builds = builder.builds;
        assert(builder.fold_xor(outer_order ? inner : &b, outer_order ? &b : inner) == &a);
        assert(builder.builds == builds);
      }
    }
  }
  for (Type type: types) {
    Value input(type);
    for (unsigned bit = 0; bit < type_width(type); ++bit) {
      Const factor(type, uint64_t(1) << bit);
      for (unsigned order = 0; order < 2; ++order) {
        ShlInst* result = dynamic_cast<ShlInst*>(builder.fold_mul(order ? &input : &factor,
                                                               order ? &factor : &input));
        assert(result && result->arg(0) == &input);
        assert(dynamic_cast<Const*>(result->arg(1))->value() == bit);
      }
    }
    Const not_power(type, 3), zero_factor(type, 0);
    assert(dynamic_cast<MulInst*>(builder.fold_mul(&input, &not_power)));
    assert(dynamic_cast<MulInst*>(builder.fold_mul(&input, &zero_factor)));
    Const shift_amount(type, type_width(type) - 1), other_amount(type, type_width(type) - 2);
    Value* sign_shift = builder.build_shr_u(&input, &shift_amount);
    LtSInst* sign_test = dynamic_cast<LtSInst*>(builder.fold_resize_x(sign_shift, Type::Bool));
    assert(sign_test && sign_test->arg(0) == &input && sign_test->type() == Type::Bool);
    Const* sign_zero = dynamic_cast<Const*>(sign_test->arg(1));
    assert(sign_zero && sign_zero->type() == type && sign_zero->value() == 0);
    Value* other_shift = builder.build_shr_u(&input, &other_amount);
    assert(dynamic_cast<ResizeXInst*>(builder.fold_resize_x(other_shift, Type::Bool)));
    Value* variable_shift = builder.build_shr_u(&input, &input);
    assert(dynamic_cast<ResizeXInst*>(builder.fold_resize_x(variable_shift, Type::Bool)));
    Value* signed_shift = builder.build_shr_s(&input, &shift_amount);
    assert(dynamic_cast<ResizeXInst*>(builder.fold_resize_x(signed_shift, Type::Bool)));
    Type other_type = type == Type::Int8 ? Type::Int16 : Type::Int8;
    assert(dynamic_cast<ResizeXInst*>(builder.fold_resize_x(sign_shift, other_type)));
    assert(dynamic_cast<Const*>(builder.fold_lt_s(&input, &input))->value() == 0);
    Const c1(type, 5), c2(type, 3);
    for (unsigned inner_order = 0; inner_order < 2; ++inner_order) {
      Value* inner_or = builder.build_or(inner_order ? &input : &c1, inner_order ? &c1 : &input);
      Value* inner_xor = builder.build_xor(inner_order ? &input : &c1, inner_order ? &c1 : &input);
      for (unsigned outer_order = 0; outer_order < 2; ++outer_order) {
        OrInst* result_or = dynamic_cast<OrInst*>(builder.fold_or(outer_order ? inner_or : &c2,
                                                                 outer_order ? &c2 : inner_or));
        XorInst* result_xor = dynamic_cast<XorInst*>(builder.fold_xor(outer_order ? inner_xor : &c2,
                                                                    outer_order ? &c2 : inner_xor));
        assert(result_or && result_or->arg(0) == &input);
        assert(dynamic_cast<Const*>(result_or->arg(1))->value() == 7);
        assert(result_xor && result_xor->arg(0) == &input);
        assert(dynamic_cast<Const*>(result_xor->arg(1))->value() == 6);
      }
    }
    Const max(type, type_mask(type)), plus_one(type, 1);
    Const* result = dynamic_cast<Const*>(builder.fold_add(&max, &plus_one));
    assert(result && result->value() == 0 && result->type() == type);
    Const* equal = dynamic_cast<Const*>(builder.fold_eq(&max, &max));
    Const* unequal = dynamic_cast<Const*>(builder.fold_eq(&max, &plus_one));
    Const* less = dynamic_cast<Const*>(builder.fold_lt_u(&plus_one, &max));
    Const* greater = dynamic_cast<Const*>(builder.fold_lt_u(&max, &plus_one));
    assert(equal && equal->type() == Type::Bool && equal->value() == 1);
    assert(unequal && unequal->type() == Type::Bool && unequal->value() == 0);
    assert(less && less->type() == Type::Bool && less->value() == 1);
    assert(greater && greater->type() == Type::Bool && greater->value() == 0);
  }
  assert(dynamic_cast<Const*>(builder.fold_sub(&x, &x))->value() == 0);
  Const same_a(Type::Int8, 7), same_b(Type::Int8, 7);
  builds = builder.builds;
  assert(dynamic_cast<Const*>(builder.fold_sub(&same_a, &same_b))->value() == 0);
  assert(builder.builds == builds + 1);
  Const* negated = dynamic_cast<Const*>(builder.fold_sub(&two, &three));
  assert(negated && negated->value() == 255);
  assert(dynamic_cast<Const*>(builder.fold_div_u(&nine, &two))->value() == 4);
  assert(dynamic_cast<DivUInst*>(builder.fold_div_u(&nine, &zero)));
  assert(dynamic_cast<Const*>(builder.fold_shl(&two, &three))->value() == 16);
  assert(dynamic_cast<ShlInst*>(builder.fold_shl(&two, &eight)));
  assert(dynamic_cast<ShlInst*>(builder.fold_mul(&x, &two)));
  AndInst* inner_and = builder.build_and(&x, &y);
  builds = builder.builds;
  assert(builder.fold_and(&x, inner_and) == inner_and);
  assert(builder.fold_and(inner_and, &x) == inner_and);
  assert(builder.builds == builds);
  ResizeUInst* wide = builder.build_resize_u(&x, Type::Int32);
  assert(builder.fold_resize_u(wide, Type::Int8) == &x);
  Value wide_input(Type::Int32);
  ResizeUInst* narrow = builder.build_resize_u(&wide_input, Type::Int8);
  assert(dynamic_cast<ResizeUInst*>(builder.fold_resize_u(narrow, Type::Int32)));
  Const* converted = dynamic_cast<Const*>(builder.fold_resize_u(&large, Type::Int64));
  assert(converted && converted->type() == Type::Int64 && converted->value() == 250);
  Value boolean(Type::Bool);
  Const bool_zero(Type::Bool, 0), bool_one(Type::Bool, 1);
  assert(dynamic_cast<Const*>(builder.fold_eq(&bool_zero, &bool_one))->value() == 0);
  assert(dynamic_cast<Const*>(builder.fold_eq(&bool_one, &bool_one))->value() == 1);
  EqInst* comparison = builder.build_eq(&two, &three);
  SelectInst* selection = dynamic_cast<SelectInst*>(builder.fold_select(comparison, &x, &y));
  assert(selection && dynamic_cast<Const*>(selection->arg(0))->type() == Type::Bool);
  assert(dynamic_cast<Const*>(selection->arg(0))->value() == 0);
  assert(dynamic_cast<ResizeSInst*>(builder.fold_resize_s(&boolean, Type::Bool)));
  assert(builder.fold_resize_s(&x, Type::Int8) == &x);
  assert(builder.fold_select(&boolean, &x, &x) == &x);
  assert(builder.fold_freeze(&one) == &one);
  assert(dynamic_cast<Const*>(builder.fold_eq(&x, &x))->value() == 1);
  #ifdef METAJIT_RULE_STATS
  bool found = false;
  for (size_t index = 0; index < sizeof(Generated::ruledsl_rule_names) / sizeof(const char*); ++index) {
    if (std::strcmp(Generated::ruledsl_rule_names[index], "add_zero") == 0) {
      assert(Generated::ruledsl_rule_counts[index] >= 2);
      found = true;
    }
  }
  assert(found);
  #endif
}
"""


class GenerationTests(unittest.TestCase):
    def test_header_regeneration_proves_before_writing_any_outputs(self):
        root = Path(__file__).resolve().parent.parent
        lwir = root.parent / "lwir.cpp"
        if not (lwir / "lwir.py").exists():
            self.skipTest("lwir.cpp is required for header-generation integration tests")
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            shutil.copyfile(root / "jitir.py", directory / "jitir.py")
            (directory / "ruledsl").mkdir()
            (directory / "ruledsl" / "fold.rules").write_text("bad: Add(x, y)\n    => Sub(x, y)")
            outputs = ("jitir.hpp", "jitir_llvmapi.hpp", "genext.hpp", "doc/jitir.md")
            (directory / "doc").mkdir()
            for output in outputs:
                (directory / output).write_text("original output\n")
            environment = dict(os.environ, PYTHONPATH=os.pathsep.join((str(root), str(lwir))))
            result = subprocess.run([sys.executable, "jitir.py"], cwd=directory, env=environment,
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("could not prove refinement", result.stderr)
            for output in outputs:
                self.assertEqual((directory / output).read_text(), "original output\n")

    def test_deterministic_output(self):
        source = "zero: Add(x, 0)\n    => x"
        self.assertEqual(generate(source), generate(source))

    def test_proof_failure_prevents_emission(self):
        with patch("ruledsl.generate.emit") as emitter:
            with self.assertRaises(ProofError):
                generate("bad: Add(x, y)\n    => Sub(x, y)")
            emitter.assert_not_called()

    def test_cli_proves_and_emits_with_handwritten_fallback(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "folds.rules"
            output = Path(directory) / "folds.inc.hpp"
            source.write_text("zero: Add(x, 0)\n    => x")
            self.assertEqual(main([str(source), "--handwritten", "Add", "-o", str(output)]), 0)
            code = output.read_text()
            self.assertIn("Value* fold_add(", code)
            self.assertIn("return _fold_add(", code)

    def test_cli_preserves_output_on_proof_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "bad.rules"
            output = Path(directory) / "folds.inc.hpp"
            source.write_text("bad: Add(x, y)\n    => Sub(x, y)")
            output.write_text("keep this output")
            errors = io.StringIO()
            with contextlib.redirect_stderr(errors):
                result = main([str(source), "-o", str(output)])
            self.assertEqual(result, 1)
            self.assertEqual(output.read_text(), "keep this output")
            self.assertIn("counterexample:", errors.getvalue())

    def test_cli_stdout(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "empty.rules"
            source.write_text("")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(main([str(source), "--handwritten", "Add"]), 0)
            self.assertIn("return _fold_add(", output.getvalue())


class NativeCppTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = shutil.which("clang++") or shutil.which("g++")
        if cls.compiler is None:
            raise unittest.SkipTest("a C++17 compiler is required for native emission tests")
        cls.code = generate(RULES, handwritten=("Add", "Sub"))

    def test_generated_folds_compile_and_run_with_and_without_statistics(self):
        fixture = Path(__file__).with_name("test_runtime.hpp")
        for statistics in (False, True):
            with self.subTest(statistics=statistics), tempfile.TemporaryDirectory() as directory:
                source = Path(directory) / "generated.cpp"
                executable = Path(directory) / "generated"
                source.write_text(f'#include "{fixture}"\nstruct Generated: RawBuilder {{\n'
                                  + self.code + "};\n" + MAIN)
                command = [self.compiler, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror",
                           "-fsanitize=undefined", "-fno-sanitize-recover=undefined",
                           str(source), "-o", str(executable)]
                if statistics:
                    command.append("-DMETAJIT_RULE_STATS")
                compiled = subprocess.run(command, capture_output=True, text=True, timeout=60)
                self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
                executed = subprocess.run([str(executable)], capture_output=True, text=True, timeout=60)
                self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)


if __name__ == "__main__":
    unittest.main()
