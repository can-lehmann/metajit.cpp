from pathlib import Path
import shutil
import unittest

from .generate import generate
from .parser import parse
from . import test_simplify


ROOT = Path(__file__).resolve().parent.parent
COUNTS = """
struct Counts {
  Value* value;
  uint8_t count;
  Value* second = nullptr;
  uint8_t second_count = 255;
  uint8_t at(Value* arg) const { return arg == value ? count : arg == second ? second_count : 255; }
};
"""


def rule_source(filename, names):
    source = (ROOT / filename).read_text()
    return "\n".join(source[rule.span.start:rule.span.end] for rule in parse(source)
                     if rule.name in names)


class ReviewGeneralizationTests(unittest.TestCase):
    run_cpp = test_simplify.SimplifyTests.run_cpp

    @classmethod
    def setUpClass(cls):
        cls.compiler = shutil.which("clang++") or shutil.which("g++")

    def test_masked_comparison(self):
        source = rule_source("fold.rules", {"eq_masked_impossible"})
        code = generate(source, include_simplify=True, simplify_folds=True)
        self.run_cpp(code, """
int main() {
  for (Type type : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Generated builder;
    Value x(type);
    Const mask(type, type == Type::Bool ? 0 : 3), impossible(type, type == Type::Bool ? 1 : 4), possible(type, 0);
    for (bool swap_and : {false, true}) {
      Inst* masked = swap_and ? builder.build_and(&mask, &x) : builder.build_and(&x, &mask);
      for (bool swap_eq : {false, true}) {
        Value* left = swap_eq ? &impossible : static_cast<Value*>(masked);
        Value* right = swap_eq ? static_cast<Value*>(masked) : &impossible;
        Const* result = dynamic_cast<Const*>(builder.fold_eq(left, right));
        assert(result && result->type() == Type::Bool && result->value() == 0);
        result = dynamic_cast<Const*>(builder.try_simplify(builder.build_eq(left, right)));
        assert(result && result->value() == 0);
      }
      assert(!builder.try_simplify(builder.build_eq(masked, &possible)));
    }
  }
}
""")

    def test_resized_select_subset(self):
        source = rule_source("fold.rules", {"or_resized_select_redundant_mask"})
        code = generate(source, include_simplify=True, simplify_folds=True)
        self.run_cpp(code, """
int main() {
  for (Type source : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    for (Type target : {Type::Bool, Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
      Generated builder;
      Value c(Type::Bool), other(Type::Bool), x(source);
      Const first(source, 5), zero(target, 0);
      Inst* selected = builder.build_select(&c, &x, &first);
      Inst* resized = builder.build_resize_u(selected, target);
      for (uint64_t bits : {uint64_t(0), uint64_t(1), uint64_t(2), uint64_t(5)}) {
        Const mask(target, bits);
        Inst* added = builder.build_select(&c, &zero, &mask);
        for (bool swapped : {false, true}) {
          Value* left = swapped ? static_cast<Value*>(added) : resized;
          Value* right = swapped ? static_cast<Value*>(resized) : added;
          size_t before = builder.allocated.size();
          Value* result = builder.fold_or(left, right);
          bool subset = ((first.value() & type_mask(target)) | mask.value()) == (first.value() & type_mask(target));
          if (subset) {
            assert(result == resized && builder.allocated.size() == before);
          } else {
            assert(dynamic_cast<OrInst*>(result));
          }
          assert(builder.try_simplify(builder.build_or(left, right)) == (subset ? resized : nullptr));
        }
        assert(!builder.try_simplify(builder.build_or(resized, builder.build_select(&other, &zero, &mask))));
      }
    }
  }
}
""")

    def test_select_add_variables(self):
        source = rule_source("simplify.rules", {"select_add_same_condition", "select_add_same_condition_unshared"})
        code = generate("", include_simplify=True, simplify_source=source)
        self.run_cpp(code, COUNTS + """
int main() {
  for (Type type : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    Generated builder;
    Value c(Type::Bool), other(Type::Bool), x(type), y(type), z(type), w(type);
    Inst* inner = builder.build_select(&c, &x, &y);
    for (bool swapped : {false, true}) {
      Inst* added = swapped ? builder.build_add(&z, inner) : builder.build_add(inner, &z);
      Inst* root = builder.build_select(&c, added, &w);
      for (uint8_t count : {0, 2, 255}) {
        size_t before = builder.allocated.size();
        assert(!builder.try_simplify(root, Generated::RuledslUnknownBits{}, Counts{added, count}));
        assert(builder.allocated.size() == before);
      }
      assert(!builder.try_simplify(root, Generated::RuledslUnknownBits{}, Counts{added, 1, inner, 2}));
      SelectInst* result = dynamic_cast<SelectInst*>(builder.try_simplify(root, Generated::RuledslUnknownBits{}, Counts{added, 1, inner, 1}));
      assert(result && result->arg(0) == &c && result->arg(2) == &w);
      AddInst* sum = dynamic_cast<AddInst*>(result->arg(1));
      assert(sum && sum->arg(0) == &x && sum->arg(1) == &z);
      assert(!builder.try_simplify(builder.build_select(&other, added, &w), Generated::RuledslUnknownBits{}, Counts{added, 1, inner, 1}));
    }
  }
}
""")

    def test_narrow_add(self):
        source = rule_source("fold.rules", {"resize_u_add_discarded_constant"})
        code = generate(source, include_simplify=True, simplify_folds=True)
        self.run_cpp(code, """
int main() {
  for (Type source : {Type::Int8, Type::Int16, Type::Int32, Type::Int64}) {
    for (Type target : {Type::Bool, Type::Int8, Type::Int16, Type::Int32}) {
      if (type_width(target) >= type_width(source)) continue;
      Generated builder;
      Value x(source);
      Const discarded(source, uint64_t(1) << type_width(target)), visible(source, 1);
      for (bool swapped : {false, true}) {
        Inst* added = swapped ? builder.build_add(&discarded, &x) : builder.build_add(&x, &discarded);
        ResizeUInst* result = dynamic_cast<ResizeUInst*>(builder.fold_resize_u(added, target));
        assert(result && result->type() == target && result->arg(0) == &x);
        result = dynamic_cast<ResizeUInst*>(builder.try_simplify(builder.build_resize_u(added, target)));
        assert(result && result->arg(0) == &x);
        assert(!builder.try_simplify(builder.build_resize_u(added, source)));
      }
      assert(!builder.try_simplify(builder.build_resize_u(builder.build_add(&x, &visible), target)));
    }
  }
}
""")
