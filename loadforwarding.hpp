#pragma once

#include <unordered_map>
#include "jitir.hpp"

namespace metajit {

class LoadForwarding: public Pass<LoadForwarding> {
private:
  using Facts = std::unordered_map<AliasingGroup, Value*>;

  static void intersect(Facts& facts, const Facts& incoming) {
    for (auto entry = facts.begin(); entry != facts.end();) {
      if (incoming.find(entry->first) == incoming.end() ||
          incoming.at(entry->first) != entry->second) {
        entry = facts.erase(entry);
      } else {
        ++entry;
      }
    }
  }

public:
  LoadForwarding(Section* section): Pass(section) {
    Incoming incoming(section);
    BlockMap<Facts> outgoing(section);
    BlockMap<size_t> remaining(section);
    NameMap<Value*> substs(section);
    bool changed = false;
    for (Block* block : *section) {
      for (Block* pred : incoming.at(block)) {
        ++remaining[pred];
      }
    }

    for (Block* block : *section) {
      const auto& preds = incoming.at(block);
      Facts facts;
      if (block != section->entry() && !preds.empty()) {
        Block* source = preds.front();
        for (Block* pred : preds) {
          if (outgoing[pred].size() < outgoing[source].size()) {
            source = pred;
          }
        }
        if (remaining[source] == 1) {
          facts.swap(outgoing[source]);
        } else {
          facts = outgoing[source];
        }
        for (Block* pred : preds) {
          if (pred != source && !facts.empty()) {
            intersect(facts, outgoing[pred]);
          }
        }
      }
      for (Block* pred : preds) {
        if (--remaining[pred] == 0) {
          Facts().swap(outgoing[pred]);
        }
      }

      for (auto it = block->begin(); it != block->end();) {
        Inst* inst = *it;
        if (dynmatch(StoreInst, store, inst)) {
          if (store->aliasing() == 0) {
            facts.clear();
          } else if (store->aliasing() < 0) {
            facts[store->aliasing()] = resolve_substitution(store->value(), substs);
          }
        } else if (dynamic_cast<CallInst*>(inst) || is_lifetime_marker(inst)) {
          facts.clear();
        } else if (dynmatch(LoadInst, load, inst)) {
          if (load->aliasing() < 0 && !load->flags().has(LoadFlags::Pure) &&
              !load->flags().has(LoadFlags::EntryPromoted)) {
            Value*& value = facts[load->aliasing()];
            if (value && value->type() == load->type()) {
              substs[load] = value;
              changed = true;
              it = it.erase();
              continue;
            }
            value = load;
          }
        }
        ++it;
      }
      if (remaining[block]) {
        outgoing[block].swap(facts);
      }
    }

    if (changed) {
      for (Block* block : *section) {
        for (Inst* inst : *block) {
          inst->substitute_args_iterated(substs);
        }
      }
    }
  }
};

}
