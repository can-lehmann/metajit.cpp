from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from .cpp import emit
from .generate import generate
from .lower_simplify import lower
from .matcher import build_matchers
from .parser import parse
from .proof import prove


RULES = """zero: Add(x, 0)
    => x
constants: Add(C1, C2)
    C = C1 + C2
    => C
reassociate: Add(Add(x, C1), C2)
    C = C1 + C2
    => Add(x, C)
subtract: Sub(x, C)
    Cneg = -C
    => Add(x, Cneg)
cancel: Xor(Xor(x, y), y)
    => x
reuse: Or(Or(x, y), x)
    => Or(x, y)
ones: Or(x, -1)
    => -1
resize: ResizeU(x: T): T
    => x
select: Select(1, x, y)
    => x
same_constants: Select(c, C, C)
    => C
"""


class SimplifyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = shutil.which("clang++") or shutil.which("g++")

    def run_cpp(self, code, main, statistics=False):
        if self.compiler is None:
            self.skipTest("a C++17 compiler is required for native emission tests")
        fixture = Path(__file__).with_name("test_runtime.hpp")
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "simplify.cpp"
            executable = Path(directory) / "simplify"
            source.write_text(f'#include "{fixture}"\nstruct Generated: RawBuilder {{\n' + code + "};\n" + main)
            command = [self.compiler, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror",
                       "-fsanitize=undefined", "-fno-sanitize-recover=undefined", str(source), "-o", str(executable)]
            if statistics:
                command.append("-DMETAJIT_RULE_STATS")
            compiled = subprocess.run(command, capture_output=True, text=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            executed = subprocess.run([str(executable)], capture_output=True, text=True, timeout=60)
            self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)

    def test_migrated_bit_operations(self):
        source = Path(__file__).with_name("fold.rules").read_text()
        rules = [rule for rule in parse(source)
                 if rule.name.startswith(("bit_reverse_", "mul_high_u_", "eq_masked_xor"))
                 or rule.name == "select_same"]
        source = "\n".join(source[rule.span.start:rule.span.end] for rule in rules)
        code = generate(source, simplify_source=source,
                        include_simplify=True, timeout_ms=10000)
        main = """
void expect_constant(Value* value, Type type, uint64_t expected) {
  Const* constant = dynamic_cast<Const*>(value);
  assert(constant && constant->type() == type && constant->value() == expected);
}
int main() {
  Generated builder;
  Value condition(Type::Bool);
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Value x(type), shift(type);
    uint64_t width = type_width(type), mask = type_mask(type);
    Const zero(type, 0), one(type, 1), two(type, 2), ones(type, mask), five(type, 5);
    for (Const* constant : {&zero, &one, &two, &ones, &five}) {
      uint64_t expected = metajit::bit_reverse(type, constant->value());
      expect_constant(builder.fold_bit_reverse(constant), type, expected);
      expect_constant(builder.try_simplify(builder.build_bit_reverse(constant)), type, expected);
      uint64_t product = uint64_t((__uint128_t(mask) * constant->value()) >> width);
      expect_constant(builder.fold_mul_high_u(&ones, constant), type, product);
      expect_constant(builder.try_simplify(builder.build_mul_high_u(&ones, constant)), type, product);
    }
    expect_constant(builder.fold_mul_high_u(&x, &zero), type, 0);
    expect_constant(builder.fold_mul_high_u(&one, &x), type, 0);
    for (uint64_t bit = 1; bit < width; ++bit) {
      Const power(type, uint64_t(1) << bit);
      ShrUInst* shifted = dynamic_cast<ShrUInst*>(builder.fold_mul_high_u(&power, &x));
      assert(shifted && shifted->arg(0) == &x);
      expect_constant(shifted->arg(1), type, width - bit);
    }
    Inst* reversed = builder.build_bit_reverse(&x);
    assert(builder.fold_bit_reverse(reversed) == &x);
    assert(builder.try_simplify(builder.build_bit_reverse(reversed)) == &x);
    unsigned before = builder.builds;
    assert(builder.try_simplify(reversed) == nullptr);
    assert(builder.builds == before);
    Value* select = builder.build_select(&condition, &one, &five);
    SelectInst* reverse_select = dynamic_cast<SelectInst*>(builder.fold_bit_reverse(select));
    assert(reverse_select && reverse_select->arg(0) == &condition);
    expect_constant(reverse_select->arg(1), type, uint64_t(1) << (width - 1));
    expect_constant(reverse_select->arg(2), type, metajit::bit_reverse(type, 5));
    SelectInst* high_select = dynamic_cast<SelectInst*>(builder.fold_mul_high_u(&ones, select));
    assert(high_select && high_select->arg(0) == &condition);
    expect_constant(high_select->arg(1), type, 0);
    expect_constant(high_select->arg(2), type, 4);
    Value* other_select = builder.build_select(&condition, &ones, &ones);
    SelectInst* high_selects = dynamic_cast<SelectInst*>(builder.fold_mul_high_u(select, other_select));
    assert(high_selects && high_selects->arg(0) == &condition);
    expect_constant(high_selects->arg(1), type, 0);
    expect_constant(high_selects->arg(2), type, 4);
    Const low_mask(type, 3), expected(type, 2), impossible(type, 4);
    Value* masked = builder.build_and(builder.build_xor(&five, &x), &low_mask);
    expect_constant(builder.fold_eq(&impossible, masked), Type::Bool, 0);
    EqInst* equal = dynamic_cast<EqInst*>(builder.try_simplify(builder.build_eq(masked, &expected)));
    assert(equal && dynamic_cast<AndInst*>(equal->arg(0)));
    expect_constant(equal->arg(1), type, 3);
  }
}
"""
        for statistics in (False, True):
            with self.subTest(statistics=statistics):
                self.run_cpp(code, main, statistics)

    def test_existing_instruction_matching(self):
        code = generate(RULES, include_simplify=True, simplify_source=RULES)
        main = """
int main() {
  Generated builder;
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Value x(type), y(type);
    Const zero(type, 0), five(type, 5), ones(type, type_mask(type));
    Inst* miss = builder.build_add(&x, &y);
    unsigned before = builder.builds;
    assert(builder.try_simplify(miss) == nullptr);
    assert(builder.builds == before);
    Inst* opaque_zero = builder.build_and(&y, &zero);
    Inst* analyzed_constant = builder.build_add(&x, opaque_zero);
    before = builder.builds;
    assert(builder.try_simplify(analyzed_constant) == nullptr);
    assert(builder.builds == before);
    assert(builder.try_simplify(builder.build_add(&x, &zero)) == &x);
    assert(builder.try_simplify(builder.build_add(&zero, &x)) == &x);
    Inst* inner = builder.build_xor(&x, &y);
    assert(builder.try_simplify(builder.build_xor(inner, &y)) == &x);
    Inst* reused = builder.build_or(&x, &y);
    Inst* reuse_root = builder.build_or(reused, &x);
    before = builder.builds;
    assert(builder.try_simplify(reuse_root) == reused);
    assert(builder.builds == before);
    Const one(type, 1);
    Inst* constant_root = builder.build_add(&ones, &one);
    Const* constant = dynamic_cast<Const*>(builder.try_simplify(constant_root));
    assert(constant && constant->type() == type && constant->value() == 0);
    assert(builder.try_simplify(builder.build_or(&x, &ones)) == &ones);
    Inst* add = builder.build_add(&x, &five);
    assert(builder.try_simplify(builder.build_sub(add, &five)) == &x);
    assert(builder.try_simplify(builder.build_resize_u(&x, type)) == &x);
    Inst* unsupported = builder.build_freeze(&x);
    before = builder.builds;
    assert(builder.try_simplify(unsupported) == nullptr);
    assert(builder.builds == before);
  }
  Value boolean(Type::Bool), other(Type::Bool);
  Const yes(Type::Bool, 1), yes_again(Type::Bool, 1);
  assert(builder.try_simplify(builder.build_xor(builder.build_xor(&boolean, &other), &other)) == &boolean);
  assert(builder.try_simplify(builder.build_select(&yes, &boolean, &other)) == &boolean);
  assert(builder.try_simplify(builder.build_select(&boolean, &yes, &yes_again)) == &yes);
}
"""
        for statistics in (False, True):
            with self.subTest(statistics=statistics):
                self.run_cpp(code, main, statistics)

    def test_backend_rule_sources_are_independent(self):
        code = generate("zero: Add(x, 0)\n    => x\n", include_simplify=True,
                        simplify_source="same: And(x, x)\n    => x\n")
        self.run_cpp(code, """
int main() {
  Generated builder;
  for (Type type : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Value x(type);
    Const zero(type, 0);
    assert(builder.fold_add(&x, &zero) == &x);
    Inst* add = builder.build_add(&x, &zero);
    Inst* same = builder.build_and(&x, &x);
    unsigned before = builder.builds;
    assert(builder.try_simplify(add) == nullptr);
    assert(builder.try_simplify(same) == &x);
    assert(builder.builds == before);
  }
  assert(Generated::ruledsl_rule_counts[0] == 5);
  assert(Generated::ruledsl_rule_counts[1] == 5);
}
""", statistics=True)

    def test_root_equivalence_reports_no_change_without_allocation(self):
        program = lower(build_matchers(prove("same: Add(x, y)\n    => Add(y, x)\n")))
        self.run_cpp(emit(program), """
int main() {
  Generated builder;
  Value x(Type::Int32), y(Type::Int32);
  Inst* root = builder.build_add(&x, &y);
  unsigned before = builder.builds;
  assert(builder.try_simplify(root) == nullptr);
  assert(builder.builds == before);
}
""")

    def test_empty_rewriter(self):
        self.run_cpp(generate("", include_simplify=True), """
int main() {
  Generated builder;
  Value x(Type::Int8), y(Type::Int8);
  assert(builder.try_simplify(builder.build_add(&x, &y)) == nullptr);
}
""")

    def test_known_bits_matching(self):
        facts_source = Path(__file__).with_name("simplify.rules").read_text()
        facts_source += ("disjoint: Add(x, y)\n    check known_zeros(x) | known_zeros(y) == -1\n    => Or(x, y)\n"
                         "subtract_or: Sub(Or(x, y), y)\n    check known_zeros(x) | known_zeros(y) == -1\n    => x\n"
                         "clear: Xor(x, C)\n    check known_ones(x) & C == C\n    Cmask = ~C\n    => And(x, Cmask)\n")
        facts_source += ("materialize: And(x, 0)\n"
                         "    Facts = known_ones(x) & 0\n    => Facts\n")
        code = generate(RULES, include_simplify=True, simplify_source=facts_source)
        main = """
struct Facts {
  Value* x;
  Value* y;
  uint64_t zeros_x;
  uint64_t ones_x;
  uint64_t zeros_y;
  uint64_t ones_y;
  uint64_t known_zeros(Value* value) const {
    return value == x ? zeros_x : value == y ? zeros_y : 0;
  }
  uint64_t known_ones(Value* value) const {
    return value == x ? ones_x : value == y ? ones_y : 0;
  }
};

int main() {
  Generated builder;
  for (Type type : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Value x(type), y(type), z(type);
    uint64_t mask = type_mask(type);
    Facts unknown{&x, &y, 0, 0, 0, 0};
    Facts redundant_and{&x, &y, mask ^ 1, 0, 0, 1};
    Inst* conjunction = builder.build_and(&x, &y);
    unsigned before = builder.builds;
    assert(builder.try_simplify(conjunction) == nullptr);
    assert(builder.try_simplify(conjunction, unknown) == nullptr);
    assert(builder.try_simplify(conjunction, redundant_and) == &x);
    assert(builder.builds == before);
    Facts redundant_or{&x, &y, 0, 1, mask ^ 1, 0};
    assert(builder.try_simplify(builder.build_or(&x, &y), redundant_or) == &x);
    Const zero(type, 0);
    Const* result = dynamic_cast<Const*>(builder.try_simplify(builder.build_and(&x, &zero), unknown));
    assert(result && result->type() == type && result->value() == 0);
    Value condition(Type::Bool);
    Facts same_constants{&x, &y, mask ^ 1, 1, mask ^ 1, 1};
    Inst* selection = builder.build_select(&condition, &x, &y);
    before = builder.builds;
    assert(builder.try_simplify(selection, same_constants) == nullptr);
    assert(builder.builds == before);
    if (type == Type::Bool) {
      continue;
    }
    Facts disjoint{&x, &y, mask ^ 1, 0, 1, 0};
    Inst* addition = builder.build_add(&x, &y);
    before = builder.builds;
    assert(builder.try_simplify(addition, unknown) == nullptr);
    assert(builder.builds == before);
    OrInst* combined = dynamic_cast<OrInst*>(builder.try_simplify(addition, disjoint));
    assert(combined && combined->arg(0) == &x && combined->arg(1) == &y);
    assert(builder.builds == before + 1);
    Inst* shared = builder.build_or(&x, &y);
    Inst* subtraction = builder.build_sub(shared, &y);
    before = builder.builds;
    assert(builder.try_simplify(subtraction, disjoint) == &x);
    assert(builder.builds == before);
    Facts drop_disjoint{&y, &z, mask ^ 1, 0, 1, 0};
    AndInst* reduced = dynamic_cast<AndInst*>(builder.try_simplify(builder.build_and(shared, &z), drop_disjoint));
    assert(reduced && reduced->arg(0) == &x && reduced->arg(1) == &z);
    Const one(type, 1);
    Facts low_one{&x, &y, 0, 1, 0, 0};
    AndInst* cleared = dynamic_cast<AndInst*>(builder.try_simplify(builder.build_xor(&x, &one), low_one));
    assert(cleared && cleared->arg(0) == &x);
    Const* complement = dynamic_cast<Const*>(cleared->arg(1));
    assert(complement && complement->type() == type && complement->value() == (mask ^ 1));
    Inst* opaque_zero = builder.build_and(&y, &zero);
    Facts constant_fact{opaque_zero, &y, mask, 0, 0, 0};
    OrInst* analyzed = dynamic_cast<OrInst*>(builder.try_simplify(builder.build_add(&x, opaque_zero), constant_fact));
    assert(analyzed && analyzed->arg(0) == &x && analyzed->arg(1) == opaque_zero);
    assert(builder.try_simplify(builder.build_add(&x, &y), redundant_and) == nullptr);
  }
}
"""
        for statistics in (False, True):
            with self.subTest(statistics=statistics):
                self.run_cpp(code, main, statistics)

    def test_migrated_select_and_resize_rules(self):
        facts_source = Path(__file__).with_name("simplify.rules").read_text()
        code = generate(RULES, include_simplify=True, simplify_source=facts_source)
        main = """
struct Facts {
  Value* value;
  uint64_t zeros;
  uint64_t ones;
  uint64_t known_zeros(Value* operand) const { return operand == value ? zeros : 0; }
  uint64_t known_ones(Value* operand) const { return operand == value ? ones : 0; }
};

int main() {
  Generated builder;
  for (Type type : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Value c(Type::Bool), x(type), y(type);
    Inst* select = builder.build_select(&c, &x, &y);
    unsigned before = builder.builds;
    assert(builder.try_simplify(select, Facts{&c, 0, 1}) == &x);
    assert(builder.try_simplify(select, Facts{&c, 1, 0}) == &y);
    assert(builder.try_simplify(select, Facts{&c, 0, 0}) == nullptr);
    assert(builder.builds == before);
    for (Type narrow : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
      if (type_width(narrow) >= type_width(type)) {
        continue;
      }
      uint64_t upper = type_mask(type) ^ type_mask(narrow);
      for (unsigned kind = 0; kind < 3; ++kind) {
        Inst* inner = kind == 0 ? (Inst*) builder.build_resize_x(&x, narrow) :
                      kind == 1 ? (Inst*) builder.build_resize_u(&x, narrow) : builder.build_resize_s(&x, narrow);
        Inst* outer = builder.build_resize_u(inner, type);
        before = builder.builds;
        assert(builder.try_simplify(outer, Facts{&x, upper, 0}) == &x);
        assert(builder.try_simplify(outer, Facts{&x, 0, 0}) == nullptr);
        assert(builder.try_simplify(outer, Facts{&x, 0, upper}) == nullptr);
        uint64_t incomplete = upper & ~(uint64_t(1) << type_width(narrow));
        assert(builder.try_simplify(outer, Facts{&x, incomplete, 0}) == nullptr);
        assert(builder.builds == before);
      }
    }
  }
}
"""
        for statistics in (False, True):
            with self.subTest(statistics=statistics):
                self.run_cpp(code, main, statistics)
