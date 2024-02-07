#ifndef SRC_SIGNATURE_JOIN_HH
#define SRC_SIGNATURE_JOIN_HH

#include "../indexing/index.hh"
#include "../similarity/signature.hh"
#include "join_algorithm.hh"

namespace join {

template <class Handler>
class SignatureJoin : public JoinAlgorithm<Handler> {
public:
  void prepare_indexing_batch(types::Batch& batch) = 0;
  void prepare_probing_batch(types::Batch& batch) = 0;
  void index_batch(types::Batch& batch) = 0;

  void join_batch(types::Batch& batch, Handler handler, statistics::JoinStatistics& statistics) = 0;
  void selfjoin_batch(types::Batch& batch, Handler handler, statistics::JoinStatistics& statistics) = 0;
};

template <class Handler>
class PrefixSignatureJoin : public SignatureJoin<Handler> {
public:
  using SetId = int64_t;

public:
  explicit PrefixSignatureJoin(similarity::Similarity& similarity)
      : similarity(*std::get<similarity::SetSimilarityPtr>(similarity)),
        prefix_signature(*std::get<similarity::SetSimilarityPtr>(similarity)) {}

  void prepare_indexing_batch(types::Batch& batch) override {
    auto& sets = std::get<types::SetBatch>(batch);

    prefix_signature.prepare_index(sets);

    int64_t universe_size = 0;
    for (auto& set : sets) {
      universe_size = std::max(universe_size, set.tokens.back());
    }
    ++universe_size;

    indexing::ComplexIndex<SetId, indexing::IndexType::DISCRETE, indexing::IndexType::ORDERED> new_index{universe_size};

    index = std::move(new_index);
  }

  void prepare_probing_batch(types::Batch& batch) override {
    auto& sets = std::get<types::SetBatch>(batch);

    prefix_signature.prepare_probe(sets);
  }

  void index_batch(types::Batch& batch) override {
    auto& sets = std::get<types::SetBatch>(batch);
    indexed_sets = sets;

    SetId set_id = 0;
    for (auto& set : sets) {
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

  void join_batch(types::Batch& batch, Handler handler, statistics::JoinStatistics& statistics) override {
    return _join_batch<false>(batch, handler, statistics);
  }

  void selfjoin_batch(types::Batch& batch, Handler handler, statistics::JoinStatistics& statistics) override {
    return _join_batch<true>(batch, handler, statistics);
  }

  template<bool is_self_join>
  void _join_batch(types::Batch& batch, Handler handler, statistics::JoinStatistics& statistics) {
    auto& sets = std::get<types::SetBatch>(batch);

    std::vector<bool> already_seen(indexed_sets.size());
    std::vector<SetId> candidates;

    for (auto& set : sets) {
      auto it = prefix_signature.begin_probing_signatures(set);
      auto it_end = prefix_signature.end_probing_signatures(set);

      auto set_size = static_cast<int64_t>(set.tokens.size());
      auto minimum_candidate_size = similarity.minimum_length_bound(set_size);
      auto maximum_candidate_size = similarity.maximum_length_bound(set_size);

      for (; it != it_end; ++it) {
        auto signature = *it;

        index.query(
          signature,
          [&](SetId set_id) {
            if (!already_seen[set_id]) {
              already_seen[set_id] = true;
              candidates.push_back(set_id);
            }
          },
          indexing::StaticNextKeyRange{minimum_candidate_size, maximum_candidate_size});
      }

      // candidate_id != candidate_set.id
      // set_id and candidate_id are internal to the join implementation only
      for (auto candidate_id : candidates) {
        // set from indexed data (indexed_sets set in index_batch)
        auto& candidate_set = indexed_sets[candidate_id];

        if constexpr (is_self_join) {
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
  types::SetBatch indexed_sets;
};

}  // namespace join

#endif  // SRC_SIGNATURE_JOIN_HH
