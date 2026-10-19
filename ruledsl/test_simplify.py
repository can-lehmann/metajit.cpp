from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from .cpp import emit
from .generate import generate
from .lower_simplify import lower
from .matcher import build_matchers
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

    def test_existing_instruction_matching(self):
        code = generate(RULES, include_simplify=True)
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
