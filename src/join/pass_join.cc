#include "pass_join.hh"

namespace join {

template <class Handler, class Filter>
void PassJoin<Handler, Filter>::prepare_indexing_batch(types::Batch& batch) {
  auto& strings = std::get<types::StringBatch>(batch);

  indexed_strings.clear();
  indexed_strings.reserve(strings.data.size());
  indexed_strings.insert(indexed_strings.begin(), strings.data.begin(), strings.data.end());

  std::sort(indexed_strings.begin(), indexed_strings.end(), [](RefString& s1, RefString& s2) {
    return s1.get() < s2.get();
  });
}

template <class Handler, class Filter>
void PassJoin<Handler, Filter>::index_batch([[maybe_unused]] types::Batch& batch) {
  // strings (or their references) already in indexed_strings
  // assert indexed_strings == batch (up to the order)

  // this is a wrapped reference == pointer, do not take reference

  int64_t id = 0;
  for (auto string_ref : indexed_strings) {
    auto& string = string_ref.get().str;

    for (auto signatures = passjoin_signature.indexing_signatures(string); const auto sig : signatures) {
      index.insert(id, static_cast<int64_t>(string.size()), sig);
    }

    ++id;
  }
}

template <class Handler, class Filter>
bool PassJoin<Handler, Filter>::has_independent_probing_signatures() { return true; }

template <class Handler, class Filter>
std::any PassJoin<Handler, Filter>::prepare_probing_batch([[maybe_unused]] types::Batch& batch) {
  auto strings = std::get<types::StringBatch>(batch);

  std::vector<CachedSignatures> signatures;
  signatures.reserve(strings.data.size());

  for (auto& string : strings.data) {
    signatures.emplace_back(passjoin_signature.cached_probing_signatures(string.str));
  }

  return signatures;
}

template <class Handler, class Filter>
void PassJoin<Handler, Filter>::join_batch(types::Batch& batch,
                Handler handler,
                statistics::JoinStatistics& statistics,
                std::shared_ptr<std::any> probing_signatures) {
  if (probing_signatures) {
    auto& signatures = std::any_cast<std::vector<CachedSignatures>&>(*probing_signatures);
    _join_batch<false>(batch, signatures, handler, statistics);
  } else {
    auto signatures = std::any_cast<std::vector<CachedSignatures>>(prepare_probing_batch(batch));
    _join_batch<false>(batch, signatures, handler, statistics);
  }
}

template <class Handler, class Filter>
void PassJoin<Handler, Filter>::selfjoin_batch(types::Batch& batch,
                    Handler handler,
                    statistics::JoinStatistics& statistics,
                    std::shared_ptr<std::any> probing_signatures) {
  if (probing_signatures) {
    auto& signatures = std::any_cast<std::vector<CachedSignatures>&>(*probing_signatures);
    _join_batch<true>(batch, signatures, handler, statistics);
  } else {
    auto signatures = std::any_cast<std::vector<CachedSignatures>>(prepare_probing_batch(batch));
    _join_batch<true>(batch, signatures, handler, statistics);
  }
}

template <class Handler, class Filter>
template <bool IS_SELF_JOIN>
void PassJoin<Handler, Filter>::_join_batch(types::Batch& batch,
                 std::vector<CachedSignatures>& cached_probing_signatures,
                 Handler handler,
                 statistics::JoinStatistics& statistics) {
  auto strings = std::get<types::StringBatch>(batch);

  std::vector<bool> already_seen(indexed_strings.size());
  std::vector<StringId> candidates;

  for (size_t i = 0; i < strings.data.size(); ++i) {
    auto& string = strings.data[i];
    auto& probing_signatures = cached_probing_signatures[i];

    KeyIterator key_iterator(static_cast<int64_t>(string.str.size()), probing_signatures);

    int64_t minimum_candidate_size = similarity.minimum_length_bound(string.str.size());
    int64_t maximum_candidate_size = similarity.maximum_length_bound(string.str.size());

    index.query(
      indexing::KeyRange(minimum_candidate_size, maximum_candidate_size),
      [&](StringId set_id) {
        if (Filter::string_pred(indexed_strings[set_id], string)) {
          if (!already_seen[set_id]) {
          already_seen[set_id] = true;
          candidates.push_back(set_id);
        }
        }
      },
      key_iterator);

    // candidate_id != candidate_set.id
    // set_id and candidate_id are internal to the join implementation only
    for (auto candidate_id : candidates) {
      // set from indexed data (indexed_sets set in index_batch)
      auto candidate_string = indexed_strings[candidate_id];

      if constexpr (IS_SELF_JOIN) {
        if (string.id <= candidate_string.get().id) {
          already_seen[candidate_id] = false;
          continue;
        }
      }

      if (similarity.is_in_threshold(candidate_string, string)) {
        handler(candidate_string.get().id, string.id);
      }

      already_seen[candidate_id] = false;
    }

    statistics.join_verifications.add(static_cast<int64_t>(candidates.size()));
    candidates.clear();
  }
}

}
