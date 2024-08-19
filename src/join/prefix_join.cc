#include "prefix_join.hh"

namespace join {

template <class Handler, class Filter>
void PrefixSignatureJoin<Handler, Filter>::insert_batch([[maybe_unused]] types::Batch& batch) {
  auto& sets = std::get<types::SetBatch>(batch);
  prefix_signature.update_frequencies(sets.data);
  bool renew_required = false;

  shared_state.totally_indexed_sets += static_cast<int64_t>(sets.data.size());
  if (shared_state.totally_indexed_sets > shared_state.next_reindexing) {
    ++shared_state.sqs_version;
    shared_state.next_reindexing = shared_state.totally_indexed_sets * 2;
  }
  if (local_sqs_version < shared_state.sqs_version) {
    renew_required = true;
    index.clear();
    preprocessed_sets.clear();

    preprocessed_sets.insert(preprocessed_sets.begin(), indexed_sets.begin(), indexed_sets.end());
    prefix_signature.convert_tokens(preprocessed_sets);
  }

  // preprocessing "consumes" the data, take copy
  preprocessed_sets.reserve(preprocessed_sets.size() + sets.data.size());
  preprocessed_sets.insert(preprocessed_sets.end(), sets.data.begin(), sets.data.end());
  indexed_sets.reserve(indexed_sets.size() + sets.data.size());
  indexed_sets.insert(indexed_sets.end(), sets.data.begin(), sets.data.end());

  types::span<types::Set> new_sets = types::span<types::Set>(preprocessed_sets.end() - static_cast<int64_t>(sets.data.size()), preprocessed_sets.end());
  prefix_signature.convert_tokens(new_sets);
  types::span<types::Set> to_index_sets = renew_required ? preprocessed_sets : new_sets;
  insert_into_index(to_index_sets);

  this->resize_bitmap(preprocessed_sets.size());
}

template <class Handler, class Filter>
void PrefixSignatureJoin<Handler, Filter>::insert_into_index(types::span<types::Set> sets) {
  for (auto& set : sets) {
    auto it = prefix_signature.begin_indexing_signatures(set);
    auto it_end = prefix_signature.end_indexing_signatures(set);

    auto set_size = set.tokens.size();

    for (; it != it_end; ++it) {
      auto signature = *it;

      index.insert(this->next_id, signature, set_size);
    }

    ++this->next_id;
  }
}

template <class Handler, class Filter>
void PrefixSignatureJoin<Handler, Filter>::join_batch(types::Batch& batch,
                                                      Handler handler,
                                                      statistics::JoinStatistics& statistics,
                                                      [[maybe_unused]] std::shared_ptr<std::any> probing_signatures) {
  return _join_batch<false>(batch, handler, statistics);
}

template <class Handler, class Filter>
void PrefixSignatureJoin<Handler, Filter>::selfjoin_batch(
  types::Batch& batch,
  Handler handler,
  statistics::JoinStatistics& statistics,
  [[maybe_unused]] std::shared_ptr<std::any> probing_signatures) {
  return _join_batch<true>(batch, handler, statistics);
}

template <class Handler, class Filter>
template <bool IS_SELF_JOIN>
void PrefixSignatureJoin<Handler, Filter>::_join_batch(types::Batch& batch,
                                                       Handler handler,
                                                       statistics::JoinStatistics& statistics) {
  auto& set_batch = std::get<types::SetBatch>(batch);

  std::vector<types::Set> prepared_probing_sets;
  if constexpr (!IS_SELF_JOIN) {
    prepared_probing_sets.reserve(set_batch.data.size());
    prepared_probing_sets.insert(prepared_probing_sets.begin(), set_batch.data.begin(), set_batch.data.end());
    prefix_signature.convert_tokens(types::span<types::Set>(prepared_probing_sets));
  }
  auto& probing_sets = IS_SELF_JOIN ? preprocessed_sets : prepared_probing_sets;

  std::vector<bool>& already_seen = this->indexed_bitmap;
  std::vector<RecordId> candidates;

  for (auto& set : probing_sets) {
    auto set_size = static_cast<int64_t>(set.tokens.size());
    auto minimum_candidate_size = similarity.minimum_length_bound(set_size);
    auto maximum_candidate_size = similarity.maximum_length_bound(set_size);

    // first find sets that might be similar due to size alone
    add_small_results(
      set, preprocessed_sets, minimum_candidate_size, maximum_candidate_size, similarity, candidates, already_seen);

    auto it = prefix_signature.begin_probing_signatures(set);
    auto it_end = prefix_signature.end_probing_signatures(set);

    for (; it != it_end; ++it) {
      auto signature = *it;

      indexing::StaticRangeIterator length_iter{std::make_pair(minimum_candidate_size, maximum_candidate_size)};
      index.query(
        signature,
        [&](RecordId set_id) {
          if (Filter::set_pred(preprocessed_sets[set_id], set)) {
            if (!already_seen[set_id]) {
              already_seen[set_id] = true;
              candidates.push_back(set_id);
            }
          }
        },
        length_iter);
    }

    // candidate_id != candidate_set.id
    // set_id and candidate_id are internal to the join implementation only
    for (auto candidate_id : candidates) {
      // set from indexed data (indexed_sets set in index_batch)
      auto& candidate_set = preprocessed_sets[candidate_id];

      if constexpr (IS_SELF_JOIN) {
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