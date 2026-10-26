#pragma once

#include <array>
#include <optional>
#include "jitir.hpp"

namespace metajit {

class BitProvenance: public Pass<BitProvenance> {
public:
  struct Stats {
    size_t visited = 0;
    size_t constants = 0;
    size_t reused = 0;
    size_t extracted = 0;
    size_t candidates = 0;
    size_t peak_fact_bytes = 0;
    uint64_t transfer_ns = 0;
    uint64_t lookup_ns = 0;
    uint64_t extraction_ns = 0;
    uint64_t remember_ns = 0;
    uint64_t cleanup_ns = 0;
    uint64_t source_extraction_ns = 0;
    uint64_t equivalence_record_ns = 0;
    uint64_t candidates_record_ns = 0;
    size_t equivalence_hits = 0;
    size_t candidate_hits = 0;
    size_t source_hits = 0;
  };

private:
  enum class ResizeKind { Unsigned, Signed, Unknown };
  enum class ShiftKind { Left, RightUnsigned, RightSigned };

  class PhaseTimer {
  private:
    uint64_t& _total;
    #ifdef METAJIT_BIT_PROVENANCE_PROFILE
    std::chrono::steady_clock::time_point _start;
    #endif
  public:
    PhaseTimer(uint64_t& total): _total(total) {
      #ifdef METAJIT_BIT_PROVENANCE_PROFILE
      _start = std::chrono::steady_clock::now();
      #endif
    }
    ~PhaseTimer() {
      #ifdef METAJIT_BIT_PROVENANCE_PROFILE
      _total += std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now() - _start)
                  .count();
      #endif
    }
  };

  typedef uint8_t Bit;

  static constexpr Bit Zero = 128;
  static constexpr Bit One = 129;
  static constexpr Bit Unknown = 130;

  struct Mapping {
    Value* source = nullptr;
    std::array<Bit, 64> bits{};

    bool constant(size_t width, uint64_t& value) const {
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
      for (size_t i = 0; i < width; i++) {
        if (bits[i] == Unknown) {
          return false;
        }
      }
      return true;
    }

    bool equal(const Mapping& other, size_t width) const {
      return source == other.source && std::memcmp(bits.data(), other.bits.data(), width) == 0;
    }

    void normalize(size_t width) {
      for (size_t i = 0; i < width; i++) {
        if (reference(bits[i])) {
          return;
        }
      }
      source = nullptr;
    }

    Mapping transfer_resize(size_t from, size_t to, ResizeKind kind) const {
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
      Mapping result = *this;
      for (size_t i = 0; i < width; i++) {
        result.bits[i] = bits[width - 1 - i];
      }
      return result;
    }

    void finish(Inst* inst, bool mixed) {
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
        result.bits[i] = bit;
        collect_source(result.source, mixed, bit_source, bit);
      }
      result.finish(inst, mixed);
      return result;
    }

    Mapping transfer_or(const Mapping& right, OrInst* inst) const {
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
        result.bits[i] = bit;
        collect_source(result.source, mixed, bit_source, bit);
      }
      result.finish(inst, mixed);
      return result;
    }

    Mapping transfer_xor(const Mapping& right, XorInst* inst) const {
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
        result.bits[i] = bit;
        collect_source(result.source, mixed, bit_source, bit);
      }
      result.finish(inst, mixed);
      return result;
    }

    Mapping transfer_lt_s(const Mapping& right, LtSInst* inst) const {
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
        result.bits[i] = bit;
        collect_source(result.source, mixed, bit_source, bit);
      }
      result.finish(inst, mixed);
      return result;
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
  NameMap<bool> _seen;
  NameMap<Value*> _substs;
  std::unordered_map<Key, Value*, Hash> _available;
  std::unordered_map<Value*, std::vector<Value*>> _candidates;
  Stats _stats;
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
    return result;
  }

  Mapping get(Value* value) const {
    if (Const* constant = dynamic_cast<Const*>(value)) {
      Mapping result;
      for (size_t i = 0; i < type_width(value->type()); i++) {
        result.bits[i] = ((constant->value() >> i) & 1) ? One : Zero;
      }
      return result;
    }
    if (value->is_named() && ((NamedValue*) value)->name() < _seen.size() &&
        _seen[(NamedValue*) value]) {
      return _facts[(NamedValue*) value];
    }
    return opaque(value);
  }

  static void collect_source(Value*& source, bool& mixed, Value* incoming, Bit bit) {
    if (!reference(bit)) {
      return;
    }
    assert(incoming);
    if (source && source != incoming) {
      mixed = true;
    }
    source = incoming;
  }

  Mapping transfer(Inst* inst) {
    PhaseTimer timer(_stats.transfer_ns);
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

  static std::optional<ResizeKind> match_resize(const Mapping& input, const Mapping& mapping,
                                                size_t from, size_t width) {
    for (ResizeKind kind : {ResizeKind::Unsigned, ResizeKind::Signed, ResizeKind::Unknown}) {
      if (input.transfer_resize(from, width, kind).equal(mapping, width)) {
        return kind;
      }
    }
    return std::nullopt;
  }

  Value* build_resize_replacement(Inst* inst, Value* input, ResizeKind kind) {
    if ((dynamic_cast<ResizeUInst*>(inst) || dynamic_cast<ResizeSInst*>(inst) ||
         dynamic_cast<ResizeXInst*>(inst)) && inst->arg(0) == input) {
      return nullptr;
    }
    if (kind == ResizeKind::Unsigned) {
      return _builder.build_resize_u(input, inst->type());
    }
    if (kind == ResizeKind::Signed) {
      return _builder.build_resize_s(input, inst->type());
    }
    return _builder.build_resize_x(input, inst->type());
  }

  struct Shift {
    ShiftKind kind;
    size_t amount;
  };

  static std::optional<Shift> match_shift(const Mapping& input, const Mapping& mapping,
                                          size_t width, size_t first, size_t position) {
    if (position >= width || position == first || input.bits[position] != mapping.bits[first]) {
      return std::nullopt;
    }
    Shift shift{first > position ? ShiftKind::Left : ShiftKind::RightUnsigned,
                first > position ? first - position : position - first};
    if (!input.transfer_shift(width, shift.amount, shift.kind).equal(mapping, width)) {
      return std::nullopt;
    }
    return shift;
  }

  Value* build_shift_replacement(Inst* inst, Value* input, Shift shift) {
    if (((shift.kind == ShiftKind::Left && dynamic_cast<ShlInst*>(inst)) ||
         (shift.kind == ShiftKind::RightUnsigned && dynamic_cast<ShrUInst*>(inst))) &&
        inst->arg(0) == input) {
      Const* count = dynamic_cast<Const*>(inst->arg(1));
      if (count && count->value() == shift.amount) {
        return nullptr;
      }
    }
    Const* count = _builder.build_const(inst->type(), shift.amount);
    return shift.kind == ShiftKind::Left ? (Value*) _builder.build_shl(input, count)
                                        : (Value*) _builder.build_shr_u(input, count);
  }

  Value* extract_from_source(Inst* inst, const Mapping& mapping) {
    PhaseTimer timer(_stats.source_extraction_ns);
    Value* source = mapping.source;
    if (!source || source == inst || !source->is_named()) {
      return nullptr;
    }
    size_t width = type_width(inst->type());
    Mapping input = get(source);
    if (source->type() != inst->type()) {
      if (auto kind = match_resize(input, mapping, type_width(source->type()), width)) {
        return build_resize_replacement(inst, source, *kind);
      }
      return nullptr;
    }
    if (input.equal(mapping, width)) {
      return source;
    }
    size_t first = 0;
    while (first < width && !reference(mapping.bits[first])) {
      first++;
    }
    if (first == width) {
      return nullptr;
    }
    if (auto shift = match_shift(input, mapping, width, first, mapping.bits[first] & 63)) {
      return build_shift_replacement(inst, source, *shift);
    }
    return nullptr;
  }

  Value* extract_from_candidates(Inst* inst, const Mapping& mapping) {
    PhaseTimer timer(_stats.extraction_ns);
    if (!mapping.source || _candidates.find(mapping.source) == _candidates.end()) {
      return nullptr;
    }
    size_t width = type_width(inst->type());
    for (Value* candidate : _candidates.at(mapping.source)) {
      _stats.candidates++;
      Mapping input = get(candidate);
      if (candidate->type() != inst->type()) {
        if (auto kind = match_resize(input, mapping, type_width(candidate->type()), width)) {
          return build_resize_replacement(inst, candidate, *kind);
        }
        continue;
      }
      size_t first = 0;
      while (first < width && !reference(mapping.bits[first])) {
        first++;
      }
      if (first == width) {
        continue;
      }
      for (size_t position = 0; position < width; position++) {
        if (auto shift = match_shift(input, mapping, width, first, position)) {
          return build_shift_replacement(inst, candidate, *shift);
        }
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
      _stats.constants++;
      return _builder.build_const(inst->type(), number);
    }
    if (!mapping.exact(type_width(inst->type()))) {
      return nullptr;
    }
    {
      PhaseTimer timer(_stats.lookup_ns);
      Key key{mapping, inst->type()};
      if (_available.find(key) != _available.end()) {
        _stats.reused++;
        _stats.equivalence_hits++;
        return _available.at(key);
      }
    }
    if (Value* replacement = extract_from_candidates(inst, mapping)) {
      _stats.extracted++;
      _stats.candidate_hits++;
      return replacement;
    }
    Value* replacement = extract_from_source(inst, mapping);
    if (replacement) {
      _stats.source_hits++;
      if (replacement == mapping.source) {
        _stats.reused++;
      } else {
        _stats.extracted++;
      }
    }
    return replacement;
  }

  void remember(NamedValue* value, const Mapping& mapping) {
    PhaseTimer timer(_stats.remember_ns);
    size_t size = _builder.next_name();
    _facts.ensure_size(size);
    _seen.ensure_size(size);
    _substs.ensure_size(size);
    _facts[value] = mapping;
    _seen[value] = true;
    _stats.peak_fact_bytes = std::max(_stats.peak_fact_bytes, _facts.size() * sizeof(Mapping));
    if (mapping.exact(type_width(value->type()))) {
      PhaseTimer record_timer(_stats.equivalence_record_ns);
      _available.try_emplace(Key{mapping, value->type()}, value);
    }
    if (mapping.source) {
      PhaseTimer record_timer(_stats.candidates_record_ns);
      std::vector<Value*>& values = _candidates[mapping.source];
      size_t count = 0;
      for (Value* previous : values) {
        if (previous->type() == value->type()) {
          count++;
        }
      }
      if (count >= 4) {
        return;
      }
      values.push_back(value);
    }
  }

public:
  BitProvenance(Section* section, const Chain& chain, Stats* stats = nullptr)
      : Pass(section), _builder(section), _facts(section), _seen(section), _substs(section) {
    _builder.set_next_name();
    for (Block* block : chain) {
      for (Arg* arg : block->args()) {
        if (is_int_or_bool(arg->type())) {
          remember(arg, opaque(arg));
        }
      }
      for (Inst* inst : *block) {
        inst->substitute_args_resolved(_substs);
        if (!is_int_or_bool(inst->type())) {
          continue;
        }
        _stats.visited++;
        Mapping mapping = transfer(inst);
        Value* replacement = find_replacement(block, inst, mapping);
        if (replacement) {
          _substs.ensure_size(_builder.next_name());
          _seen.ensure_size(_builder.next_name());
          _substs[inst] = replacement;
          if (replacement->is_inst() && !_seen[(NamedValue*) replacement]) {
            remember((NamedValue*) replacement, mapping);
          }
        } else {
          remember(inst, mapping);
        }
      }
    }
    {
      PhaseTimer timer(_stats.cleanup_ns);
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
    if (stats) {
      stats->visited += _stats.visited;
      stats->constants += _stats.constants;
      stats->reused += _stats.reused;
      stats->extracted += _stats.extracted;
      stats->candidates += _stats.candidates;
      stats->peak_fact_bytes = std::max(stats->peak_fact_bytes, _stats.peak_fact_bytes);
      stats->transfer_ns += _stats.transfer_ns;
      stats->lookup_ns += _stats.lookup_ns;
      stats->extraction_ns += _stats.extraction_ns;
      stats->remember_ns += _stats.remember_ns;
      stats->cleanup_ns += _stats.cleanup_ns;
      stats->source_extraction_ns += _stats.source_extraction_ns;
      stats->equivalence_record_ns += _stats.equivalence_record_ns;
      stats->candidates_record_ns += _stats.candidates_record_ns;
      stats->equivalence_hits += _stats.equivalence_hits;
      stats->candidate_hits += _stats.candidate_hits;
      stats->source_hits += _stats.source_hits;
    }
  }
};

}
