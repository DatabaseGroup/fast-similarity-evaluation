#ifndef ALGORITHM_RESOLUTION_HH
#define ALGORITHM_RESOLUTION_HH

#include <memory>

#include "../ontology/planner.hh"
#include "execution_cache.hh"
#include "palloc_join.hh"
#include "pass_join.hh"
#include "prefix_join.hh"
#include "result_handler.hh"
#include "t_join.hh"

namespace join {

template <class Handler = MaterializeHandler>
struct AlgorithmSharedState {
  typename PrefixSignatureJoin<Handler>::SharedState prefix;
  typename PallocJoin<Handler, true>::SharedState palloc;
  typename PallocJoin<Handler, false>::SharedState partition;
};

inline std::unique_ptr<JoinAlgorithm<MaterializeHandler>> resolve_algorithmid(
  AlgorithmId id,
  similarity::Similarity& similarity,
  AlgorithmSharedState<MaterializeHandler>& shared_state) {
  switch (id) {
  case PREFIX_SIGNATURE_JOIN:
    return std::make_unique<PrefixSignatureJoin<MaterializeHandler>>(similarity, shared_state.prefix);
  case FALLBACK:
    // todo implement comparing all pairs as obvious fallback
    break;
  case PASS_JOIN:
    return std::make_unique<PassJoin<MaterializeHandler>>(similarity);
  case TJOIN:
    return std::make_unique<TJoinLite<MaterializeHandler>>(similarity);
  case PALLOC:
    return std::make_unique<PallocJoin<MaterializeHandler, true>>(similarity, shared_state.palloc);
  case PARTITION:
    return std::make_unique<PallocJoin<MaterializeHandler, false>>(similarity, shared_state.partition);
  }
  return std::make_unique<PrefixSignatureJoin<MaterializeHandler>>(similarity, shared_state.prefix);
}

template <class DataType, class SimilarityPtr>
void _verify(DataType& dataset, SimilarityPtr& similarity, std::vector<types::ResultPair>& pairs) {
  pairs.erase(std::remove_if(pairs.begin(),
                             pairs.end(),
                             [&](auto& pair) {
                               auto l_id = pair.first;
                               auto r_id = pair.second;
                               auto& l = dataset.data[l_id];
                               auto& r = dataset.data[r_id];
                               return !similarity->is_in_threshold(l, r);
                             }),
              pairs.end());
}

template <class DataType, class SimilarityPtr>
void _offset_verify(DataType& left_dataset,
                    int64_t left_offset,
                    DataType& right_dataset,
                    int64_t right_offset,
                    SimilarityPtr& similarity,
                    std::vector<types::ResultPair>& pairs) {
  pairs.erase(std::remove_if(pairs.begin(),
                             pairs.end(),
                             [&](auto& pair) {
                               auto l_id = pair.first;
                               auto r_id = pair.second;
                               auto& l = left_dataset.data[l_id - left_offset];
                               auto& r = right_dataset.data[r_id - right_offset];
                               return !similarity->is_in_threshold(l, r);
                             }),
              pairs.end());
}

inline void verify_with_similarity(types::Dataset& dataset,
                                   similarity::Similarity& similarity,
                                   std::vector<types::ResultPair>& pairs) {
  auto set_verify = [&](types::Sets& sets) {
    _verify(sets, std::get<similarity::SetSimilarityPtr>(similarity), pairs);
  };
  auto string_verify = [&](types::Strings& strings) {
    _verify(strings, std::get<similarity::StringSimilarityPtr>(similarity), pairs);
  };
  auto tree_verify = [&](types::Trees& trees) {
    _verify(trees, std::get<similarity::TreeSimilarityPtr>(similarity), pairs);
  };

  std::visit(util::overloaded{set_verify, string_verify, tree_verify}, dataset);
}

inline void offset_verify_with_similarity(types::Batch& left_dataset,
                                          int64_t left_offset,
                                          types::Batch& right_dataset,
                                          int64_t right_offset,
                                          similarity::Similarity& similarity,
                                          std::vector<types::ResultPair>& pairs) {
  auto set_verify = [&](types::SetBatch& left_sets) {
    _offset_verify(left_sets,
                   left_offset,
                   std::get<types::SetBatch>(right_dataset),
                   right_offset,
                   std::get<similarity::SetSimilarityPtr>(similarity),
                   pairs);
  };
  auto string_verify = [&](types::StringBatch& left_strings) {
    _offset_verify(left_strings,
                   left_offset,
                   std::get<types::StringBatch>(right_dataset),
                   right_offset,
                   std::get<similarity::StringSimilarityPtr>(similarity),
                   pairs);
  };
  auto tree_verify = [&](types::TreeBatch& left_trees) {
    _offset_verify(left_trees,
                   left_offset,
                   std::get<types::TreeBatch>(right_dataset),
                   right_offset,
                   std::get<similarity::TreeSimilarityPtr>(similarity),
                   pairs);
  };

  std::visit(util::overloaded{set_verify, string_verify, tree_verify}, left_dataset);
}

inline types::Batch dataset_to_batch(types::Dataset& dataset,
                                     size_t start = 0,
                                     size_t end = std::numeric_limits<size_t>::max()) {
  return std::visit(
    [&](auto&& data) {
      using DatasetType = std::decay_t<decltype(data)>;
      auto real_end = std::min(data.data.size(), end);
      return types::Batch(types::DataBatch<typename DatasetType::value_type>(
        types::span<typename DatasetType::value_type>(data.data.begin() + start, data.data.begin() + real_end),
        data.meta));
    },
    dataset);
}

inline types::Batch get_batch_by_offset(types::Dataset& dataset, const int64_t start, const int64_t end) {
  return std::visit(
    [&](auto&& actual_dataset) {
      using DatasetType = std::decay_t<decltype(actual_dataset)>;
      return types::Batch(types::DataBatch<typename DatasetType::value_type>(
        types::span<typename DatasetType::value_type>(
          actual_dataset.data.begin() + start, std::min(actual_dataset.data.begin() + end, actual_dataset.data.end())),
        actual_dataset.meta));
    },
    dataset);
}

inline types::Batch get_batch_by_id(types::Dataset& dataset, const int64_t batch_idx, const int64_t batch_size) {
  int64_t offset = batch_idx * batch_size;
  return get_batch_by_offset(dataset, offset, offset + batch_size);
}
inline int64_t get_offset_into_batch(int64_t batch_idx, int64_t batch_size) { return batch_idx * batch_size; }

inline void verify_pairs_for_plan(types::Dataset& dataset,
                                  std::vector<std::shared_ptr<types::Dataset>>& reduced_datasets,
                                  similarity::Similarity& similarity,
                                  ontology::QueryPlan& plan,
                                  types::ResultPairs& result_pairs,
                                  int64_t index_offset,
                                  int64_t probe_offset,
                                  IndexedBatch& probe_batch,
                                  ReductionCache& reduction_cache,
                                  statistics::LocalJoinStatistics& plan_statistics) {
  if (!plan.steps.empty()) {
    auto similarities = reduction_cache.get_all_reduced_similarities(similarity, plan);

    for (int32_t level = 1; level < static_cast<int32_t>(plan.steps.size()); ++level) {
      auto reduced_index = *reduced_datasets[level];
      auto reduced_index_batch = dataset_to_batch(reduced_index);
      auto reduced_probe =
        reduction_cache.reduce_data_to_level(probe_batch, plan, level, plan_statistics.rc_statistics);
      auto reduced_probe_batch = dataset_to_batch(*reduced_probe);

      plan_statistics.step_verifications[plan.steps.size() - (level + 1)].add(
        static_cast<int64_t>(result_pairs.size()));
      offset_verify_with_similarity(
        reduced_index_batch, index_offset, reduced_probe_batch, probe_offset, similarities[level], result_pairs);
    }

    // if data was actually reduced, we still have to verify with the "outermost" similarity
    // otherwise, the algorithm instance has already verified this part
    plan_statistics.step_verifications.back().add(static_cast<int64_t>(result_pairs.size()));
    verify_with_similarity(dataset, similarity, result_pairs);
  }
}

}  // namespace join

#endif  // ALGORITHM_RESOLUTION_HH
