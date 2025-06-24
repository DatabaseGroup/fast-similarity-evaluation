#include "pass_join.hh"

#include "../util/object_ptr.hh"

namespace join {

template <class Handler, bool PRESORTED>
void PassJoin<Handler, PRESORTED>::insert_batch(types::Batch& indexed_data, types::Batch& batch) {
  auto& indexed_strings = std::get<types::StringBatch>(indexed_data).data;
  auto& strings = std::get<types::StringBatch>(batch);
  this->resize_bitmap(indexed_strings.size());

  for (auto& string : strings.data) {

    for (auto signatures = passjoin_signature.indexing_signatures(string.str); const auto sig : signatures) {
      index.insert(this->next_id, static_cast<int64_t>(string.str.size()), sig);
    }

    ++this->next_id;
  }
}

template <class Handler, bool PRESORTED>
bool PassJoin<Handler, PRESORTED>::has_independent_probing_signatures() {
  return true;
}

template <class Handler, bool PRESORTED>
std::any PassJoin<Handler, PRESORTED>::get_probing_signatures([[maybe_unused]] types::Batch& batch) {
  auto strings = std::get<types::StringBatch>(batch);

  std::vector<CachedSignatures> signatures;
  signatures.reserve(strings.data.size());

  for (auto& string : strings.data) {
    signatures.emplace_back(passjoin_signature.cached_probing_signatures(string.str));
  }

  return signatures;
}

template <class Handler, bool PRESORTED>
void PassJoin<Handler, PRESORTED>::join_batch(types::Batch& indexed_data,
                                  types::Batch& batch,
                                   Handler handler,
                                   FilterConfig& filter_config,
                                   statistics::JoinStatistics& statistics,
                                   std::shared_ptr<std::any> probing_signatures) {
  // CachedSignatures is not default-constructible
  util::object_ptr<std::vector<CachedSignatures>> signatures;
  std::vector<CachedSignatures> local_signatures;
  if (probing_signatures) {
    signatures = &std::any_cast<std::vector<CachedSignatures>&>(*probing_signatures);
  } else {
    local_signatures = std::any_cast<std::vector<CachedSignatures>>(get_probing_signatures(batch));
    signatures = &local_signatures;
  }

  // this could be done in a nicer way
  switch (filter_config.type) {
  case NOP:
    _join_batch<NopFilter>(indexed_data, batch, *signatures, handler, filter_config, statistics);
    break;
  case SIMPLE_SELFJOIN:
    _join_batch<SimpleSelfjoinFilter>(indexed_data, batch, *signatures, handler, filter_config, statistics);
    break;
  case SYMMETRIC_PAIRS:
    _join_batch<SymmetricPairFilter>(indexed_data, batch, *signatures, handler, filter_config, statistics);
    break;
  case CUTOFF:
    _join_batch<CutoffFilter>(indexed_data, batch, *signatures, handler, filter_config, statistics);
    break;
  case CUTOFF_SELFJOIN:
    _join_batch<CutoffSelfFilter>(indexed_data, batch, *signatures, handler, filter_config, statistics);
    break;
  }
}

template <class Handler, bool PRESORTED>
template <class Filter>
void PassJoin<Handler, PRESORTED>::_join_batch(types::Batch& indexed_data, types::Batch& batch,
                                    std::vector<CachedSignatures>& cached_probing_signatures,
                                    Handler handler,
                                    FilterConfig& filter_config,
                                    statistics::JoinStatistics& statistics) {
  auto& indexed_strings = std::get<types::StringBatch>(indexed_data).data;
  auto& strings = std::get<types::StringBatch>(batch);

  std::vector<bool>& already_seen = this->indexed_bitmap;
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
        if (Filter::scan_break_cond(indexed_strings[set_id], string, filter_config)) {
          return true;
        }
        if (!Filter::scan_skip_cond(indexed_strings[set_id], string, filter_config)) {
          if (!already_seen[set_id]) {
            already_seen[set_id] = true;
            candidates.push_back(set_id);
          }
        }
        return false;
      },
      key_iterator);

    // candidate_id != candidate_set.id
    // set_id and candidate_id are internal to the join implementation only
    for (auto candidate_id : candidates) {
      // set from indexed data (indexed_sets set in index_batch)
      auto candidate_string = indexed_strings[candidate_id];

      if constexpr (Filter::literally_selfjoin()) {
        if (string.id <= candidate_string.id) {
          already_seen[candidate_id] = false;
          continue;
        }
      }

      if (similarity.is_in_threshold(candidate_string, string)) {
        handler(candidate_string.id, string.id);
      }

      already_seen[candidate_id] = false;
    }

    statistics.join_verifications.add(static_cast<int64_t>(candidates.size()));
    candidates.clear();
  }
}

}  // namespace join
