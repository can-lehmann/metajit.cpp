#pragma once

#include <array>
#include "jitir.hpp"

namespace metajit {

class BitProvenance: public Pass<BitProvenance> {
public:
  enum class ExtractionMode { AvailableValues, SourceOnly, EquivalenceOnly, CandidatesOnly, FirstFour };

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
  enum class BinaryKind { And, Or, Xor };

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
      _total += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _start).count();
      #endif
    }
  };

  static constexpr uint8_t Zero = 128;
  static constexpr uint8_t One = 129;
  static constexpr uint8_t Unknown = 130;

  struct Mapping {
    Value* source = nullptr;
    std::array<uint8_t, 64> bits{};
  };

  struct Key {
    Mapping mapping;
    Type type;

    bool operator==(const Key& other) const {
      return type == other.type && mapping.source == other.mapping.source &&
        std::memcmp(mapping.bits.data(), other.mapping.bits.data(), type_width(type)) == 0;
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
  ExtractionMode _mode;

  bool use_equivalence() const {
    return _mode == ExtractionMode::AvailableValues || _mode == ExtractionMode::EquivalenceOnly || _mode == ExtractionMode::FirstFour;
  }

  bool use_candidates() const {
    return _mode == ExtractionMode::AvailableValues || _mode == ExtractionMode::CandidatesOnly || _mode == ExtractionMode::FirstFour;
  }

  static bool reference(uint8_t bit) { return bit < Zero; }

  static uint8_t invert(uint8_t bit) {
    if (reference(bit)) { return bit ^ 64; }
    return bit == Zero ? One : bit == One ? Zero : Unknown;
  }

  static bool same(uint8_t a, Value* as, uint8_t b, Value* bs) {
    return a != Unknown && b != Unknown && a == b && (!reference(a) || as == bs);
  }

  static Mapping opaque(Value* value) {
    Mapping result;
    result.source = value;
    for (size_t i = 0; i < type_width(value->type()); i++) { result.bits[i] = i; }
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
    if (value->is_named() && ((NamedValue*) value)->name() < _seen.size() && _seen[(NamedValue*) value]) {
      return _facts[(NamedValue*) value];
    }
    return opaque(value);
  }

  static bool constant(const Mapping& mapping, size_t width, uint64_t& value) {
    value = 0;
    for (size_t i = 0; i < width; i++) {
      if (mapping.bits[i] == One) { value |= uint64_t(1) << i; }
      else if (mapping.bits[i] != Zero) { return false; }
    }
    return true;
  }

  static bool exact(const Mapping& mapping, size_t width) {
    for (size_t i = 0; i < width; i++) {
      if (mapping.bits[i] == Unknown) { return false; }
    }
    return true;
  }

  static bool equal(const Mapping& a, const Mapping& b, size_t width) {
    return a.source == b.source && std::memcmp(a.bits.data(), b.bits.data(), width) == 0;
  }

  static void normalize(Mapping& mapping, size_t width) {
    for (size_t i = 0; i < width; i++) {
      if (reference(mapping.bits[i])) { return; }
    }
    mapping.source = nullptr;
  }

  static Mapping resize(const Mapping& input, size_t from, size_t to, unsigned kind) {
    Mapping result = input;
    uint8_t fill = kind == 0 ? Zero : kind == 1 ? input.bits[from - 1] : Unknown;
    for (size_t i = from; i < to; i++) { result.bits[i] = fill; }
    normalize(result, to);
    return result;
  }

  static Mapping shift(const Mapping& input, size_t width, uint64_t amount, unsigned kind) {
    Mapping result;
    result.source = input.source;
    for (size_t i = 0; i < width; i++) {
      if (amount >= width) { result.bits[i] = Unknown; }
      else if (kind == 0) { result.bits[i] = i < amount ? Zero : input.bits[i - amount]; }
      else { result.bits[i] = i + amount < width ? input.bits[i + amount] : kind == 1 ? Zero : input.bits[width - 1]; }
    }
    normalize(result, width);
    return result;
  }

  static void collect_source(Value*& source, bool& mixed, Value* incoming, uint8_t bit) {
    if (!reference(bit)) { return; }
    assert(incoming);
    if (source && source != incoming) { mixed = true; }
    source = incoming;
  }

  static Mapping finish(Mapping result, Inst* inst, bool mixed) {
    size_t width = type_width(inst->type());
    if (mixed) {
      result.source = inst;
      for (size_t i = 0; i < width; i++) {
        if (result.bits[i] != Zero && result.bits[i] != One) { result.bits[i] = i; }
      }
    }
    normalize(result, width);
    return result;
  }

  static Mapping binary(const Mapping& left, const Mapping& right, Inst* inst, BinaryKind kind) {
    Mapping result;
    bool mixed = false;
    for (size_t i = 0; i < type_width(inst->type()); i++) {
      uint8_t a = left.bits[i], b = right.bits[i], bit = Unknown;
      Value* source = nullptr;
      bool eq = same(a, left.source, b, right.source);
      bool opposite = same(a, left.source, invert(b), right.source);
      if (kind == BinaryKind::And && (a == Zero || b == Zero || opposite)) { bit = Zero; }
      else if (kind == BinaryKind::Or && (a == One || b == One || opposite)) { bit = One; }
      else if (kind == BinaryKind::Xor && eq) { bit = Zero; }
      else if (kind == BinaryKind::Xor && opposite) { bit = One; }
      else if (a == Zero || a == One) {
        bit = kind == BinaryKind::Xor && a == One ? invert(b) : b;
        source = right.source;
      } else if (b == Zero || b == One) {
        bit = kind == BinaryKind::Xor && b == One ? invert(a) : a;
        source = left.source;
      } else if (eq) { bit = a; source = left.source; }
      else if (a != Unknown && b != Unknown) { bit = i; source = inst; }
      result.bits[i] = bit;
      collect_source(result.source, mixed, source, bit);
    }
    return finish(result, inst, mixed);
  }

  Mapping transfer(Inst* inst) {
    PhaseTimer timer(_stats.transfer_ns);
    size_t width = type_width(inst->type());
    if (inst->has_side_effect() || dynamic_cast<LoadInst*>(inst) || inst->arg_count() == 0 ||
        !is_int_or_bool(inst->arg(0)->type())) { return opaque(inst); }
    Mapping a = get(inst->arg(0));
    if (dynamic_cast<ResizeUInst*>(inst)) { return resize(a, type_width(inst->arg(0)->type()), width, 0); }
    if (dynamic_cast<ResizeSInst*>(inst)) { return resize(a, type_width(inst->arg(0)->type()), width, 1); }
    if (dynamic_cast<ResizeXInst*>(inst)) { return resize(a, type_width(inst->arg(0)->type()), width, 2); }
    if (dynamic_cast<BitReverseInst*>(inst)) {
      Mapping result = a;
      for (size_t i = 0; i < width; i++) { result.bits[i] = a.bits[width - 1 - i]; }
      return result;
    }
    if (inst->arg_count() < 2 || !is_int_or_bool(inst->arg(1)->type())) { return opaque(inst); }
    Mapping b = get(inst->arg(1));
    if (dynamic_cast<AndInst*>(inst)) { return binary(a, b, inst, BinaryKind::And); }
    if (dynamic_cast<OrInst*>(inst)) { return binary(a, b, inst, BinaryKind::Or); }
    if (dynamic_cast<XorInst*>(inst)) { return binary(a, b, inst, BinaryKind::Xor); }
    uint64_t number = 0;
    if (dynamic_cast<ShlInst*>(inst) || dynamic_cast<ShrUInst*>(inst) || dynamic_cast<ShrSInst*>(inst)) {
      if (!constant(b, width, number)) { return opaque(inst); }
      unsigned kind = dynamic_cast<ShlInst*>(inst) ? 0 : dynamic_cast<ShrUInst*>(inst) ? 1 : 2;
      return shift(a, width, number, kind);
    }
    if (dynamic_cast<LtSInst*>(inst) && constant(b, type_width(inst->arg(1)->type()), number) && number == 0) {
      Mapping result;
      result.source = a.source;
      result.bits[0] = a.bits[type_width(inst->arg(0)->type()) - 1];
      normalize(result, 1);
      return result;
    }
    if (dynamic_cast<EqInst*>(inst)) {
      size_t from = type_width(inst->arg(0)->type());
      Mapping result;
      if (exact(a, from) && exact(b, from) && equal(a, b, from)) { result.bits[0] = One; return result; }
      for (size_t i = 0; i < from; i++) {
        if (same(a.bits[i], a.source, invert(b.bits[i]), b.source)) { result.bits[0] = Zero; return result; }
      }
    }
    if (dynamic_cast<SelectInst*>(inst)) {
      if (constant(a, 1, number)) { return get(inst->arg(number ? 1 : 2)); }
      Mapping c = get(inst->arg(2)), result;
      bool mixed = false;
      for (size_t i = 0; i < width; i++) {
        uint8_t bit;
        Value* source;
        if (same(b.bits[i], b.source, c.bits[i], c.source)) { bit = b.bits[i]; source = b.source; }
        else if (b.bits[i] >= Zero && b.bits[i] <= One && c.bits[i] >= Zero && c.bits[i] <= One) {
          bit = b.bits[i] == One ? a.bits[0] : invert(a.bits[0]); source = a.source;
        } else { bit = i; source = inst; }
        result.bits[i] = bit;
        collect_source(result.source, mixed, source, bit);
      }
      return finish(result, inst, mixed);
    }
    return opaque(inst);
  }

  Value* source_extraction(Inst* inst, const Mapping& mapping) {
    PhaseTimer timer(_stats.source_extraction_ns);
    Value* source = mapping.source;
    if (!source || source == inst || !source->is_named()) { return nullptr; }
    size_t width = type_width(inst->type());
    size_t from = type_width(source->type());
    Mapping input = get(source);
    if (source->type() != inst->type()) {
      for (unsigned kind = 0; kind < 3; kind++) {
        if (!equal(resize(input, from, width, kind), mapping, width)) { continue; }
        if ((dynamic_cast<ResizeUInst*>(inst) || dynamic_cast<ResizeSInst*>(inst) || dynamic_cast<ResizeXInst*>(inst)) && inst->arg(0) == source) { return nullptr; }
        if (kind == 0) { return _builder.build_resize_u(source, inst->type()); }
        if (kind == 1) { return _builder.build_resize_s(source, inst->type()); }
        return _builder.build_resize_x(source, inst->type());
      }
      return nullptr;
    }
    if (equal(input, mapping, width)) { return source; }
    size_t first = 0;
    while (first < width && !reference(mapping.bits[first])) { first++; }
    if (first == width) { return nullptr; }
    size_t position = mapping.bits[first] & 63;
    if (position >= width || position == first || input.bits[position] != mapping.bits[first]) { return nullptr; }
    unsigned kind = first > position ? 0 : 1;
    size_t amount = first > position ? first - position : position - first;
    if (!equal(shift(input, width, amount, kind), mapping, width)) { return nullptr; }
    if (((kind == 0 && dynamic_cast<ShlInst*>(inst)) || (kind == 1 && dynamic_cast<ShrUInst*>(inst))) && inst->arg(0) == source) {
      Const* count = dynamic_cast<Const*>(inst->arg(1));
      if (count && count->value() == amount) { return nullptr; }
    }
    Const* count = _builder.build_const(inst->type(), amount);
    return kind == 0 ? (Value*) _builder.build_shl(source, count) : (Value*) _builder.build_shr_u(source, count);
  }

  Value* extraction(Inst* inst, const Mapping& mapping) {
    PhaseTimer timer(_stats.extraction_ns);
    if (!mapping.source || _candidates.find(mapping.source) == _candidates.end()) { return nullptr; }
    size_t width = type_width(inst->type());
    for (Value* candidate : _candidates.at(mapping.source)) {
      _stats.candidates++;
      Mapping input = get(candidate);
      size_t from = type_width(candidate->type());
      if (candidate->type() != inst->type()) {
        for (unsigned kind = 0; kind < 3; kind++) {
          if (!equal(resize(input, from, width, kind), mapping, width)) { continue; }
          if ((dynamic_cast<ResizeUInst*>(inst) || dynamic_cast<ResizeSInst*>(inst) || dynamic_cast<ResizeXInst*>(inst)) && inst->arg(0) == candidate) { return nullptr; }
          if (kind == 0) { return _builder.build_resize_u(candidate, inst->type()); }
          if (kind == 1) { return _builder.build_resize_s(candidate, inst->type()); }
          return _builder.build_resize_x(candidate, inst->type());
        }
        continue;
      }
      size_t first = 0;
      while (first < width && !reference(mapping.bits[first])) { first++; }
      if (first == width) { continue; }
      for (size_t position = 0; position < width; position++) {
        if (input.bits[position] != mapping.bits[first] || position == first) { continue; }
        unsigned kind = first > position ? 0 : 1;
        size_t amount = first > position ? first - position : position - first;
        if (!equal(shift(input, width, amount, kind), mapping, width)) { continue; }
        if (((kind == 0 && dynamic_cast<ShlInst*>(inst)) || (kind == 1 && dynamic_cast<ShrUInst*>(inst))) && inst->arg(0) == candidate) {
          Const* count = dynamic_cast<Const*>(inst->arg(1));
          if (count && count->value() == amount) { return nullptr; }
        }
        Const* count = _builder.build_const(inst->type(), amount);
        return kind == 0 ? (Value*) _builder.build_shl(candidate, count) : (Value*) _builder.build_shr_u(candidate, count);
      }
    }
    return nullptr;
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
    if (use_equivalence() && exact(mapping, type_width(value->type()))) {
      PhaseTimer record_timer(_stats.equivalence_record_ns);
      _available.try_emplace(Key{mapping, value->type()}, value);
    }
    if (use_candidates() && mapping.source) {
      PhaseTimer record_timer(_stats.candidates_record_ns);
      std::vector<Value*>& values = _candidates[mapping.source];
      if (_mode == ExtractionMode::FirstFour) {
        size_t count = 0;
        for (Value* previous : values) {
          if (previous->type() == value->type()) { count++; }
        }
        if (count >= 4) { return; }
      }
      values.push_back(value);
    }
  }

public:
  BitProvenance(Section* section, const Chain& chain, Stats* stats = nullptr, ExtractionMode mode = ExtractionMode::AvailableValues):
      Pass(section), _builder(section), _facts(section), _seen(section), _substs(section), _mode(mode) {
    _builder.set_next_name();
    for (Block* block : chain) {
      for (Arg* arg : block->args()) {
        if (is_int_or_bool(arg->type())) { remember(arg, opaque(arg)); }
      }
      for (Inst* inst : *block) {
        inst->substitute_args_resolved(_substs);
        if (!is_int_or_bool(inst->type())) { continue; }
        _stats.visited++;
        Mapping mapping = transfer(inst);
        Value* replacement = nullptr;
        if (!inst->has_side_effect() && !inst->is_terminator() && !dynamic_cast<LoadInst*>(inst)) {
          uint64_t number;
          _builder.move_before(block, inst);
          if (constant(mapping, type_width(inst->type()), number)) {
            replacement = _builder.build_const(inst->type(), number);
            _stats.constants++;
          } else if (exact(mapping, type_width(inst->type()))) {
            if (use_equivalence()) {
              PhaseTimer timer(_stats.lookup_ns);
              Key key{mapping, inst->type()};
              if (_available.find(key) != _available.end()) {
                replacement = _available.at(key);
                _stats.reused++;
                _stats.equivalence_hits++;
              }
            }
            if (!replacement && use_candidates()) {
              replacement = extraction(inst, mapping);
              if (replacement) { _stats.extracted++; _stats.candidate_hits++; }
            }
            if (!replacement) {
              replacement = source_extraction(inst, mapping);
              if (replacement) {
                _stats.source_hits++;
                if (replacement == mapping.source) { _stats.reused++; }
                else { _stats.extracted++; }
              }
            }
          }
        }
        if (replacement) {
          _substs.ensure_size(_builder.next_name());
          _seen.ensure_size(_builder.next_name());
          _substs[inst] = replacement;
          if (replacement->is_inst() && !_seen[(NamedValue*) replacement]) { remember((NamedValue*) replacement, mapping); }
        } else { remember(inst, mapping); }
      }
    }
    {
      PhaseTimer timer(_stats.cleanup_ns);
      for (Block* block : *section) {
        for (Inst* inst : *block) { inst->substitute_args_resolved(_substs); }
        for (auto cursor = block->begin(); cursor != block->end(); ) {
          if (_substs[*cursor]) { cursor = cursor.erase(); }
          else { cursor++; }
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
