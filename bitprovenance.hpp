#pragma once

#include <array>
#include <optional>
#include "jitir.hpp"

namespace metajit {

class BitProvenance: public Pass<BitProvenance> {
private:
  enum class ResizeKind { Unsigned, Signed, Unknown };
  enum class ShiftKind { Left, RightUnsigned, RightSigned };

  struct Shift {
    ShiftKind kind;
    size_t amount;
  };

  typedef uint8_t Bit;

  static constexpr Bit Zero = 128;
  static constexpr Bit One = 129;
  static constexpr Bit Unknown = 130;
  static constexpr Bit Invalid = 131;

  struct Mapping {
    Value* source = nullptr;
    std::array<Bit, 64> bits{Invalid};

    bool valid() const { return bits[0] != Invalid; }

    void validate(size_t width) const {
      #ifdef METAJIT_SLOW_ASSERTS
      assert(valid());
      assert(width > 0 && width <= bits.size());
      bool has_reference = false;
      for (size_t i = 0; i < width; i++) {
        assert(bits[i] <= Unknown);
        if (reference(bits[i])) {
          assert(source && is_int_or_bool(source->type()));
          assert((bits[i] & 63) < type_width(source->type()));
          has_reference = true;
        }
      }
      assert(has_reference == (source != nullptr));
      #endif
    }

    bool constant(size_t width, uint64_t& value) const {
      assert(valid());
      value = 0;
      for (size_t i = 0; i < width; i++) {
        if (bits[i] == One) {
          value |= uint64_t(1) << i;
        } else if (bits[i] != Zero) {
          return false;
        }
      }
      return true;
    }

    bool exact(size_t width) const {
      assert(valid());
      for (size_t i = 0; i < width; i++) {
        if (bits[i] == Unknown) {
          return false;
        }
      }
      return true;
    }

    bool equal(const Mapping& other, size_t width) const {
      assert(valid() && other.valid());
      return source == other.source && std::memcmp(bits.data(), other.bits.data(), width) == 0;
    }

    void normalize(size_t width) {
      assert(valid());
      for (size_t i = 0; i < width; i++) {
        if (reference(bits[i])) {
          return;
        }
      }
      source = nullptr;
    }

    Mapping transfer_resize(size_t from, size_t to, ResizeKind kind) const {
      assert(valid());
      Mapping result = *this;
      Bit fill = kind == ResizeKind::Unsigned ? Zero :
                 kind == ResizeKind::Signed ? bits[from - 1] : Unknown;
      for (size_t i = from; i < to; i++) {
        result.bits[i] = fill;
      }
      result.normalize(to);
      return result;
    }

    Mapping transfer_shift(size_t width, uint64_t amount, ShiftKind kind) const {
      assert(valid());
      Mapping result;
      result.source = source;
      for (size_t i = 0; i < width; i++) {
        if (amount >= width) {
          result.bits[i] = Unknown;
        } else if (kind == ShiftKind::Left) {
          result.bits[i] = i < amount ? Zero : bits[i - amount];
        } else {
          result.bits[i] = i + amount < width ? bits[i + amount] :
                           kind == ShiftKind::RightUnsigned ? Zero : bits[width - 1];
        }
      }
      result.normalize(width);
      return result;
    }

    Mapping transfer_bit_reverse(size_t width) const {
      assert(valid());
      Mapping result = *this;
      for (size_t i = 0; i < width; i++) {
        result.bits[i] = bits[width - 1 - i];
      }
      return result;
    }

    void set_bit(size_t index, Bit bit, Value* bit_source, bool& mixed) {
      bits[index] = bit;
      if (!reference(bit)) {
        return;
      }
      assert(bit_source);
      if (source && source != bit_source) {
        mixed = true;
      }
      source = bit_source;
    }

    void finish(Inst* inst, bool mixed) {
      assert(valid());
      size_t width = type_width(inst->type());
      if (mixed) {
        source = inst;
        for (size_t i = 0; i < width; i++) {
          if (bits[i] != Zero && bits[i] != One) {
            bits[i] = i;
          }
        }
      }
      normalize(width);
    }

    Mapping transfer_and(const Mapping& right, AndInst* inst) const {
      assert(valid() && right.valid());
      Mapping result;
      bool mixed = false;
      for (size_t i = 0; i < type_width(inst->type()); i++) {
        Bit a = bits[i], b = right.bits[i], bit = Unknown;
        Value* bit_source = nullptr;
        bool eq = same(a, this->source, b, right.source);
        bool opposite = same(a, this->source, invert(b), right.source);
        if (a == Zero || b == Zero || opposite) {
          bit = Zero;
        } else if (a == One) {
          bit = b;
          bit_source = right.source;
        } else if (b == One) {
          bit = a;
          bit_source = this->source;
        } else if (eq) {
          bit = a;
          bit_source = this->source;
        } else if (a != Unknown && b != Unknown) {
          bit = i;
          bit_source = inst;
        }
        result.set_bit(i, bit, bit_source, mixed);
      }
      result.finish(inst, mixed);
      return result;
    }

    Mapping transfer_or(const Mapping& right, OrInst* inst) const {
      assert(valid() && right.valid());
      Mapping result;
      bool mixed = false;
      for (size_t i = 0; i < type_width(inst->type()); i++) {
        Bit a = bits[i], b = right.bits[i], bit = Unknown;
        Value* bit_source = nullptr;
        bool eq = same(a, this->source, b, right.source);
        bool opposite = same(a, this->source, invert(b), right.source);
        if (a == One || b == One || opposite) {
          bit = One;
        } else if (a == Zero) {
          bit = b;
          bit_source = right.source;
        } else if (b == Zero) {
          bit = a;
          bit_source = this->source;
        } else if (eq) {
          bit = a;
          bit_source = this->source;
        } else if (a != Unknown && b != Unknown) {
          bit = i;
          bit_source = inst;
        }
        result.set_bit(i, bit, bit_source, mixed);
      }
      result.finish(inst, mixed);
      return result;
    }

    Mapping transfer_xor(const Mapping& right, XorInst* inst) const {
      assert(valid() && right.valid());
      Mapping result;
      bool mixed = false;
      for (size_t i = 0; i < type_width(inst->type()); i++) {
        Bit a = bits[i], b = right.bits[i], bit = Unknown;
        Value* bit_source = nullptr;
        bool eq = same(a, this->source, b, right.source);
        bool opposite = same(a, this->source, invert(b), right.source);
        if (eq) {
          bit = Zero;
        } else if (opposite) {
          bit = One;
        } else if (a == Zero) {
          bit = b;
          bit_source = right.source;
        } else if (a == One) {
          bit = invert(b);
          bit_source = right.source;
        } else if (b == Zero) {
          bit = a;
          bit_source = this->source;
        } else if (b == One) {
          bit = invert(a);
          bit_source = this->source;
        } else if (a != Unknown && b != Unknown) {
          bit = i;
          bit_source = inst;
        }
        result.set_bit(i, bit, bit_source, mixed);
      }
      result.finish(inst, mixed);
      return result;
    }

    Mapping transfer_lt_s(const Mapping& right, LtSInst* inst) const {
      assert(valid() && right.valid());
      uint64_t number;
      if (!right.constant(type_width(inst->arg(1)->type()), number) || number != 0) {
        return opaque(inst);
      }
      Mapping result;
      result.source = source;
      result.bits[0] = bits[type_width(inst->arg(0)->type()) - 1];
      result.normalize(1);
      return result;
    }

    Mapping transfer_eq(const Mapping& right, EqInst* inst) const {
      assert(valid() && right.valid());
      size_t width = type_width(inst->arg(0)->type());
      Mapping result;
      if (exact(width) && right.exact(width) && equal(right, width)) {
        result.bits[0] = One;
        return result;
      }
      for (size_t i = 0; i < width; i++) {
        if (same(bits[i], source, invert(right.bits[i]), right.source)) {
          result.bits[0] = Zero;
          return result;
        }
      }
      return opaque(inst);
    }

    Mapping transfer_select(const Mapping& true_value, const Mapping& false_value,
                            SelectInst* inst) const {
      assert(valid() && true_value.valid() && false_value.valid());
      Mapping result;
      bool mixed = false;
      for (size_t i = 0; i < type_width(inst->type()); i++) {
        Bit bit;
        Value* bit_source;
        if (same(true_value.bits[i], true_value.source, false_value.bits[i], false_value.source)) {
          bit = true_value.bits[i];
          bit_source = true_value.source;
        } else if (true_value.bits[i] >= Zero && true_value.bits[i] <= One &&
                   false_value.bits[i] >= Zero && false_value.bits[i] <= One) {
          bit = true_value.bits[i] == One ? bits[0] : invert(bits[0]);
          bit_source = source;
        } else {
          bit = i;
          bit_source = inst;
        }
        result.set_bit(i, bit, bit_source, mixed);
      }
      result.finish(inst, mixed);
      return result;
    }

    std::optional<ResizeKind> match_resize(const Mapping& target, size_t from, size_t width) const {
      assert(target.exact(width));
      for (ResizeKind kind : {ResizeKind::Unsigned, ResizeKind::Signed}) {
        if (transfer_resize(from, width, kind).equal(target, width)) {
          return kind;
        }
      }
      return std::nullopt;
    }

    std::optional<Shift> match_shift(const Mapping& target, size_t width,
                                    size_t first, size_t position) const {
      assert(valid() && target.valid());
      if (position >= width || position == first || bits[position] != target.bits[first]) {
        return std::nullopt;
      }
      Shift shift{first > position ? ShiftKind::Left : ShiftKind::RightUnsigned,
                  first > position ? first - position : position - first};
      if (!transfer_shift(width, shift.amount, shift.kind).equal(target, width)) {
        return std::nullopt;
      }
      return shift;
    }
  };

  struct Key {
    Mapping mapping;
    Type type;

    bool operator==(const Key& other) const {
      return type == other.type && mapping.equal(other.mapping, type_width(type));
    }
  };

  struct Hash {
    size_t operator()(const Key& key) const {
      size_t hash = std::hash<Value*>()(key.mapping.source) ^ size_t(key.type);
      for (size_t i = 0; i < type_width(key.type); i++) {
        hash = (hash ^ key.mapping.bits[i]) * 1099511628211ull;
      }
      return hash;
    }
  };

  Builder _builder;
  NameMap<Mapping> _facts;
  NameMap<Value*> _substs;
  std::unordered_map<Key, Value*, Hash> _available;
  std::unordered_map<Value*, std::vector<Value*>> _candidates;
  static constexpr size_t MaxCandidatesPerSourceType = 4;
  static bool reference(Bit bit) { return bit < Zero; }

  static Bit invert(Bit bit) {
    if (reference(bit)) {
      return bit ^ 64;
    }
    return bit == Zero ? One : bit == One ? Zero : Unknown;
  }

  static bool same(Bit a, Value* as, Bit b, Value* bs) {
    return a != Unknown && b != Unknown && a == b && (!reference(a) || as == bs);
  }

  static Mapping opaque(Value* value) {
    Mapping result;
    result.source = value;
    for (size_t i = 0; i < type_width(value->type()); i++) {
      result.bits[i] = i;
    }
    result.validate(type_width(value->type()));
    return result;
  }

  Mapping get(Value* value) const {
    if (Const* constant = dynamic_cast<Const*>(value)) {
      Mapping result;
      for (size_t i = 0; i < type_width(value->type()); i++) {
        result.bits[i] = ((constant->value() >> i) & 1) ? One : Zero;
      }
      result.validate(type_width(value->type()));
      return result;
    }
    if (value->is_named() && ((NamedValue*) value)->name() < _facts.size() &&
        _facts[(NamedValue*) value].valid()) {
      return _facts[(NamedValue*) value];
    }
    return opaque(value);
  }

  Mapping transfer(Inst* inst) {
    size_t width = type_width(inst->type());
    if (inst->has_side_effect() || dynamic_cast<LoadInst*>(inst) || inst->arg_count() == 0 ||
        !is_int_or_bool(inst->arg(0)->type())) {
      return opaque(inst);
    }
    Mapping a = get(inst->arg(0));
    if (dynamic_cast<ResizeUInst*>(inst)) {
      return a.transfer_resize(type_width(inst->arg(0)->type()), width, ResizeKind::Unsigned);
    }
    if (dynamic_cast<ResizeSInst*>(inst)) {
      return a.transfer_resize(type_width(inst->arg(0)->type()), width, ResizeKind::Signed);
    }
    if (dynamic_cast<ResizeXInst*>(inst)) {
      return a.transfer_resize(type_width(inst->arg(0)->type()), width, ResizeKind::Unknown);
    }
    if (dynamic_cast<BitReverseInst*>(inst)) {
      return a.transfer_bit_reverse(width);
    }
    if (inst->arg_count() < 2 || !is_int_or_bool(inst->arg(1)->type())) {
      return opaque(inst);
    }
    Mapping b = get(inst->arg(1));
    if (AndInst* and_inst = dynamic_cast<AndInst*>(inst)) {
      return a.transfer_and(b, and_inst);
    }
    if (OrInst* or_inst = dynamic_cast<OrInst*>(inst)) {
      return a.transfer_or(b, or_inst);
    }
    if (XorInst* xor_inst = dynamic_cast<XorInst*>(inst)) {
      return a.transfer_xor(b, xor_inst);
    }
    uint64_t number = 0;
    if (dynamic_cast<ShlInst*>(inst) || dynamic_cast<ShrUInst*>(inst) ||
        dynamic_cast<ShrSInst*>(inst)) {
      if (!b.constant(width, number)) {
        return opaque(inst);
      }
      ShiftKind kind = dynamic_cast<ShlInst*>(inst) ? ShiftKind::Left :
                       dynamic_cast<ShrUInst*>(inst) ? ShiftKind::RightUnsigned :
                       ShiftKind::RightSigned;
      return a.transfer_shift(width, number, kind);
    }
    if (LtSInst* lt_s = dynamic_cast<LtSInst*>(inst)) {
      return a.transfer_lt_s(b, lt_s);
    }
    if (EqInst* eq = dynamic_cast<EqInst*>(inst)) {
      return a.transfer_eq(b, eq);
    }
    if (SelectInst* select = dynamic_cast<SelectInst*>(inst)) {
      if (a.constant(1, number)) {
        return get(select->arg(number ? 1 : 2));
      }
      return a.transfer_select(b, get(select->arg(2)), select);
    }
    return opaque(inst);
  }

  Value* build_resize_replacement(Inst* inst, Value* input, ResizeKind kind) {
    if ((dynamic_cast<ResizeUInst*>(inst) || dynamic_cast<ResizeSInst*>(inst) ||
         dynamic_cast<ResizeXInst*>(inst)) && inst->arg(0) == input) {
      return inst;
    }
    if (kind == ResizeKind::Unsigned) {
      return _builder.build_resize_u(input, inst->type());
    }
    assert(kind == ResizeKind::Signed);
    return _builder.build_resize_s(input, inst->type());
  }

  Value* build_shift_replacement(Inst* inst, Value* input, Shift shift) {
    if (((shift.kind == ShiftKind::Left && dynamic_cast<ShlInst*>(inst)) ||
         (shift.kind == ShiftKind::RightUnsigned && dynamic_cast<ShrUInst*>(inst))) &&
        inst->arg(0) == input) {
      Const* count = dynamic_cast<Const*>(inst->arg(1));
      if (count && count->value() == shift.amount) {
        return inst;
      }
    }
    Const* count = _builder.build_const(inst->type(), shift.amount);
    return shift.kind == ShiftKind::Left ? (Value*) _builder.build_shl(input, count)
                                        : (Value*) _builder.build_shr_u(input, count);
  }

  Value* extract_from_value(Inst* inst, const Mapping& mapping, Value* value) {
    size_t width = type_width(inst->type());
    Mapping input = get(value);
    if (value->type() != inst->type()) {
      if (auto kind = input.match_resize(mapping, type_width(value->type()), width)) {
        return build_resize_replacement(inst, value, *kind);
      }
      return nullptr;
    }
    if (input.equal(mapping, width)) {
      return value;
    }
    size_t first = 0;
    while (first < width && !reference(mapping.bits[first])) {
      first++;
    }
    if (first == width) {
      return nullptr;
    }
    if (value == mapping.source) {
      if (auto shift = input.match_shift(mapping, width, first, mapping.bits[first] & 63)) {
        return build_shift_replacement(inst, value, *shift);
      }
    } else {
      for (size_t position = 0; position < width; position++) {
        if (auto shift = input.match_shift(mapping, width, first, position)) {
          return build_shift_replacement(inst, value, *shift);
        }
      }
    }
    return nullptr;
  }

  Value* extract_from_source(Inst* inst, const Mapping& mapping) {
    Value* source = mapping.source;
    if (!source || source == inst || !source->is_named()) {
      return nullptr;
    }
    return extract_from_value(inst, mapping, source);
  }

  Value* extract_from_candidates(Inst* inst, const Mapping& mapping) {
    if (!mapping.source || _candidates.find(mapping.source) == _candidates.end()) {
      return nullptr;
    }
    for (Value* candidate : _candidates.at(mapping.source)) {
      if (Value* replacement = extract_from_value(inst, mapping, candidate)) {
        return replacement;
      }
    }
    return nullptr;
  }

  Value* find_replacement(Block* block, Inst* inst, const Mapping& mapping) {
    if (inst->has_side_effect() || inst->is_terminator() || dynamic_cast<LoadInst*>(inst)) {
      return nullptr;
    }
    _builder.move_before(block, inst);
    uint64_t number;
    if (mapping.constant(type_width(inst->type()), number)) {
      return _builder.build_const(inst->type(), number);
    }
    if (!mapping.exact(type_width(inst->type()))) {
      return nullptr;
    }
    Key key{mapping, inst->type()};
    if (_available.find(key) != _available.end()) {
      return _available.at(key);
    }
    Value* replacement = extract_from_source(inst, mapping);
    if (replacement && replacement != inst) {
      return replacement;
    }
    replacement = extract_from_candidates(inst, mapping);
    if (replacement == inst) {
      return nullptr;
    }
    return replacement;
  }

  void record_value(NamedValue* value, const Mapping& mapping) {
    assert(mapping.valid());
    mapping.validate(type_width(value->type()));
    size_t size = _builder.next_name();
    _facts.ensure_size(size);
    _substs.ensure_size(size);
    _facts[value] = mapping;
    if (mapping.exact(type_width(value->type()))) {
      _available.try_emplace(Key{mapping, value->type()}, value);
    }
    if (mapping.source) {
      std::vector<Value*>& values = _candidates[mapping.source];
      size_t count = 0;
      for (Value* previous : values) {
        if (previous->type() == value->type()) {
          count++;
        }
      }
      if (count >= MaxCandidatesPerSourceType) {
        return;
      }
      values.push_back(value);
    }
  }

public:
  BitProvenance(Section* section, const Chain& chain)
      : Pass(section), _builder(section), _facts(section), _substs(section) {
    _builder.set_next_name();
    for (Block* block : chain) {
      for (Arg* arg : block->args()) {
        if (is_int_or_bool(arg->type())) {
          record_value(arg, opaque(arg));
        }
      }
      for (Inst* inst : *block) {
        inst->substitute_args_resolved(_substs);
        if (!is_int_or_bool(inst->type())) {
          continue;
        }
        Mapping mapping = transfer(inst);
        mapping.validate(type_width(inst->type()));
        Value* replacement = find_replacement(block, inst, mapping);
        if (replacement) {
          _substs.ensure_size(_builder.next_name());
          _facts.ensure_size(_builder.next_name());
          _substs[inst] = replacement;
          if (replacement->is_inst() && !_facts[(NamedValue*) replacement].valid()) {
            record_value((NamedValue*) replacement, mapping);
          }
        } else {
          record_value(inst, mapping);
        }
      }
    }
    for (Block* block : *section) {
      for (Inst* inst : *block) {
        inst->substitute_args_resolved(_substs);
      }
      for (auto cursor = block->begin(); cursor != block->end();) {
        if (_substs[*cursor]) {
          cursor = cursor.erase();
        } else {
          cursor++;
        }
      }
    }
  }
};

}
