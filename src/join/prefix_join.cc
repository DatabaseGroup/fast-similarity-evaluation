#include "prefix_join.hh"

namespace join {

template <class Handler>
void PrefixSignatureJoin<Handler>::insert_batch(types::Batch& indexed_data, types::Batch& batch) {
  auto& indexed_sets = std::get<types::SetBatch>(indexed_data).data;
  auto& sets = std::get<types::SetBatch>(batch);

  // the frequencies might become biased if the same set is indexed multiple times
  prefix_signature.update_frequencies(sets.data);

  std::for_each(sets.data.begin(), sets.data.end(), [&](const auto& set) {
    shared_state.totally_indexed_tokens += set.tokens.size();
  });
  if (shared_state.totally_indexed_tokens > shared_state.next_reindexing) {
    shared_state.sqs.build_token_mapping();
    ++shared_state.sqs_version;
    shared_state.next_reindexing = shared_state.totally_indexed_tokens * 4;
  }
  if (local_sqs_version < shared_state.sqs_version) {
    update_index(indexed_sets);
  } else {
    // preprocessing "consumes" the data, take copy
    preprocessed_sets.insert(preprocessed_sets.end(), sets.data.begin(), sets.data.end());

    types::span<types::Set> new_sets = types::span<types::Set>(
      preprocessed_sets.end() - static_cast<int64_t>(sets.data.size()), preprocessed_sets.end());
    prefix_signature.convert_tokens(new_sets);
    insert_into_index(new_sets);
  }

  this->resize_bitmap(preprocessed_sets.size());
}

template <class Handler>
void PrefixSignatureJoin<Handler>::insert_into_index(types::span<types::Set> sets) {
  int64_t max_asbs = similarity.max_asbs();

  for (auto& set : sets) {
    auto it = prefix_signature.begin_indexing_signatures(set);
    auto it_end = prefix_signature.end_indexing_signatures(set);

    auto set_size = set.tokens.size();

    for (; it != it_end; ++it) {
      auto signature = *it;

      index.insert(this->next_id, signature, set_size);
    }

    if (static_cast<int64_t>(set_size) <= max_asbs) {
      small_index.emplace(static_cast<int32_t>(set_size), static_cast<int32_t>(this->next_id));
    }

    ++this->next_id;
  }
}

template <class Handler>
std::any PrefixSignatureJoin<Handler>::get_probing_signatures(types::Batch& batch) {
  CachedSignatures cs;

  auto& set_batch = std::get<types::SetBatch>(batch);
  cs.prepared_sets.reserve(set_batch.data.size());
  cs.prepared_sets.insert(cs.prepared_sets.begin(), set_batch.data.begin(), set_batch.data.end());
  prefix_signature.convert_tokens(types::span<types::Set>(cs.prepared_sets));

  cs.sqs_version = shared_state.sqs_version;
  return cs;
}

template <class Handler>
void PrefixSignatureJoin<Handler>::update_index(types::span<types::Set>& indexed_sets) {
  index.clear();
  small_index.clear();
  preprocessed_sets.clear();
  this->next_id = 0;
  local_sqs_version = shared_state.sqs_version;

  preprocessed_sets.insert(preprocessed_sets.begin(), indexed_sets.begin(), indexed_sets.end());
  prefix_signature.convert_tokens(preprocessed_sets);

  insert_into_index(preprocessed_sets);
}

template <class Handler>
void PrefixSignatureJoin<Handler>::join_batch(types::Batch& indexed_data,
                                              types::Batch& batch,
                                              Handler handler,
                                              FilterConfig& filter_config,
                                              statistics::JoinStatistics& statistics,
                                              std::shared_ptr<std::any> probing_signatures) {
  if (local_sqs_version < shared_state.sqs_version) {
    auto& indexed_sets = std::get<types::SetBatch>(indexed_data).data;
    update_index(indexed_sets);
  }
  util::object_ptr<CachedSignatures> signatures;
  CachedSignatures local_signatures;
  if (probing_signatures) {
    auto& cached_signatures = std::any_cast<CachedSignatures&>(*probing_signatures);
    if (cached_signatures.sqs_version != shared_state.sqs_version) {
      local_signatures = std::any_cast<CachedSignatures>(get_probing_signatures(batch));
      cached_signatures.prepared_sets = std::move(local_signatures.prepared_sets);
      cached_signatures.sqs_version = shared_state.sqs_version;
    }
    signatures = &cached_signatures;
  } else {
    local_signatures = std::any_cast<CachedSignatures>(get_probing_signatures(batch));
    signatures = &local_signatures;
  }

  // this could be done in a nicer way
  switch (filter_config.type) {
  case NOP:
    _join_batch<NopFilter>(*signatures, handler, filter_config, statistics);
    break;
  case SIMPLE_SELFJOIN:
    _join_batch<SimpleSelfjoinFilter>(*signatures, handler, filter_config, statistics);
    break;
  case SYMMETRIC_PAIRS:
    _join_batch<SymmetricPairFilter>(*signatures, handler, filter_config, statistics);
    break;
  case CUTOFF:
    _join_batch<CutoffFilter>(*signatures, handler, filter_config, statistics);
    break;
  case CUTOFF_SELFJOIN:
    _join_batch<CutoffSelfFilter>(*signatures, handler, filter_config, statistics);
    break;
  }
}

template <class Handler>
template <class Filter>
void PrefixSignatureJoin<Handler>::_join_batch(CachedSignatures& signatures,
                                               Handler handler,
                                               FilterConfig& filter_config,
                                               statistics::JoinStatistics& statistics) {
  auto& probing_sets = Filter::literally_selfjoin() ? preprocessed_sets : signatures.prepared_sets;

  std::vector<bool>& already_seen = this->indexed_bitmap;
  std::vector<RecordId> candidates;

  for (auto& set : probing_sets) {
    auto set_size = static_cast<int64_t>(set.tokens.size());
    auto minimum_candidate_size = similarity.minimum_length_bound(set_size);
    auto maximum_candidate_size = similarity.maximum_length_bound(set_size);

    // first find sets that might be similar due to size alone
    add_small_results<Filter>(set,
                              small_index.begin(),
                              small_index.end(),
                              preprocessed_sets,
                              minimum_candidate_size,
                              maximum_candidate_size,
                              similarity,
                              candidates,
                              filter_config,
                              already_seen);

    auto it = prefix_signature.begin_probing_signatures(set);
    auto it_end = prefix_signature.end_probing_signatures(set);
    int64_t pos = 0;

    for (; it != it_end; ++it) {
      auto signature = *it;
      auto maximum_candidate_size_pel = similarity.maximum_length_pel(set_size, pos);

      indexing::StaticRangeIterator length_iter{std::make_pair(minimum_candidate_size, maximum_candidate_size_pel)};
      index.query(
        signature,
        [&](RecordId set_id) {
          if (!Filter::scan_break_cond(preprocessed_sets[set_id], set, filter_config) &&
              !Filter::scan_skip_cond(preprocessed_sets[set_id], set, filter_config)) {
            if (!already_seen[set_id]) {
              already_seen[set_id] = true;
              candidates.push_back(set_id);
            }
          }
          return false;
        },
        length_iter);
      ++pos;
    }

    // candidate_id != candidate_set.id
    // set_id and candidate_id are internal to the join implementation only
    for (auto candidate_id : candidates) {
      // set from indexed data (indexed_sets set in index_batch)
      auto& candidate_set = preprocessed_sets[candidate_id];

      if constexpr (Filter::literally_selfjoin()) {
        if (set.id <= candidate_set.id) {
          already_seen[candidate_id] = false;
          continue;
        }
      }

      if (similarity.is_in_threshold(candidate_set, set)) {
        handler(candidate_set.id, set.id);
      }

      already_seen[candidate_id] = false;
    }
    statistics.join_verifications.add(static_cast<int64_t>(candidates.size()));
    candidates.clear();
  }
}

}  // namespace join