#ifndef SRC_PREFIX_JOIN_HH
#define SRC_PREFIX_JOIN_HH

#include "../similarity/similarity.hh"
#include "signature_join.hh"

namespace join {

// Used to support add_small_results for PrefixSignature
template <>
struct SizeGetter<types::Set> {
  static int64_t get_size(types::Set& set) { return static_cast<int64_t>(set.tokens.size()); }
};

template <class Handler>
class PrefixSignatureJoin : public SignatureJoin<Handler> {
public:
  explicit PrefixSignatureJoin(similarity::Similarity& similarity)
      : similarity(*std::get<similarity::SetSimilarityPtr>(similarity)),
        prefix_signature(*std::get<similarity::SetSimilarityPtr>(similarity)) {}

  void prepare_indexing_batch(types::Batch& batch) override {
    // this "consumes" the data, take copy
    auto& sets = std::get<types::SetBatch>(batch);
    indexed_sets.reserve(sets.data.size());
    indexed_sets.insert(indexed_sets.begin(), sets.data.begin(), sets.data.end());

    prefix_signature.prepare_index(indexed_sets);

    int64_t universe_size = 0;
    for (auto& set : indexed_sets) {
      universe_size = std::max(universe_size, set.tokens.back());
    }
    ++universe_size;

    indexing::ComplexIndex<SetId, indexing::IndexType::DISCRETE, indexing::IndexType::ORDERED> new_index{universe_size};

    index = std::move(new_index);
  }

  void index_batch(types::Batch& batch) override {
    // assert batch == indexed_Sets

    SetId set_id = 0;
    for (auto& set : indexed_sets) {
      auto it = prefix_signature.begin_indexing_signatures(set);
      auto it_end = prefix_signature.end_indexing_signatures(set);

      auto set_size = set.tokens.size();

      for (; it != it_end; ++it) {
        auto signature = *it;

        index.insert(set_id, signature, set_size);
      }

      ++set_id;
    }
  }

  void join_batch(types::Batch& batch,
                  Handler handler,
                  statistics::JoinStatistics& statistics,
                  [[maybe_unused]] std::shared_ptr<std::any> probing_signatures) override {
    return _join_batch<false>(batch, handler, statistics);
  }

  void selfjoin_batch(types::Batch& batch,
                      Handler handler,
                      statistics::JoinStatistics& statistics,
                      [[maybe_unused]] std::shared_ptr<std::any> probing_signatures) override {
    return _join_batch<true>(batch, handler, statistics);
  }

  template <bool IS_SELF_JOIN>
  void _join_batch(types::Batch& batch, Handler handler, statistics::JoinStatistics& statistics) {
    auto& set_batch = std::get<types::SetBatch>(batch);

    std::vector<types::Set> prepared_probing_sets;
    if constexpr (!IS_SELF_JOIN) {
      prepared_probing_sets.reserve(set_batch.data.size());
      prepared_probing_sets.insert(prepared_probing_sets.begin(), set_batch.data.begin(), set_batch.data.end());
      prefix_signature.prepare_probe(prepared_probing_sets);
    }
    auto& probing_sets = IS_SELF_JOIN ? indexed_sets : prepared_probing_sets;

    std::vector<bool> already_seen(indexed_sets.size());
    std::vector<SetId> candidates;

    for (auto& set : probing_sets) {
      auto set_size = static_cast<int64_t>(set.tokens.size());
      auto minimum_candidate_size = similarity.minimum_length_bound(set_size);
      auto maximum_candidate_size = similarity.maximum_length_bound(set_size);

      // first find sets that might be similar due to size alone
      add_small_results(
        set, indexed_sets, minimum_candidate_size, maximum_candidate_size, similarity, candidates, already_seen);

      auto it = prefix_signature.begin_probing_signatures(set);
      auto it_end = prefix_signature.end_probing_signatures(set);

      for (; it != it_end; ++it) {
        auto signature = *it;

        indexing::StaticRangeIterator length_iter{std::make_pair(minimum_candidate_size, maximum_candidate_size)};
        index.query(
          signature,
          [&](SetId set_id) {
            if (!already_seen[set_id]) {
              already_seen[set_id] = true;
              candidates.push_back(set_id);
            }
          },
          length_iter);
      }

      // candidate_id != candidate_set.id
      // set_id and candidate_id are internal to the join implementation only
      for (auto candidate_id : candidates) {
        // set from indexed data (indexed_sets set in index_batch)
        auto& candidate_set = indexed_sets[candidate_id];

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

private:
  similarity::SetSimilarity& similarity;
  similarity::SetPrefixSignature prefix_signature;
  indexing::ComplexIndex<SetId, indexing::IndexType::DISCRETE, indexing::IndexType::ORDERED> index{0};
  std::vector<types::Set> indexed_sets;
};

}

#endif  // SRC_PREFIX_JOIN_HH
