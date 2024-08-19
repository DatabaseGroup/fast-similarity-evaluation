#ifndef ALGORITHM_RESOLUTION_HH
#define ALGORITHM_RESOLUTION_HH

#include "../ontology/planner.hh"
#include "execution_cache.hh"
#include "palloc_join.hh"
#include "pass_join.hh"
#include "prefix_join.hh"
#include "result_handler.hh"
#include "t_join.hh"

namespace join {

template <class Handler = MaterializeHandler, class Filter = NopFilter>
struct AlgorithmSharedState {
  typename PrefixSignatureJoin<Handler, Filter>::SharedState prefix;
  typename PallocJoin<Handler, Filter>::SharedState palloc;
};

template <class Filter = NopFilter>
std::unique_ptr<JoinAlgorithm<MaterializeHandler, Filter>> resolve_algorithmid(
  AlgorithmId id,
  similarity::Similarity& similarity,
  AlgorithmSharedState<MaterializeHandler, Filter>& shared_state) {
  switch (id) {
  case PREFIX_SIGNATURE_JOIN:
    return std::make_unique<PrefixSignatureJoin<MaterializeHandler, Filter>>(similarity, shared_state.prefix);
  case FALLBACK:
    // todo implement comparing all pairs as obvious fallback
    break;
  case PASS_JOIN:
    return std::make_unique<PassJoin<MaterializeHandler, Filter>>(similarity);
  case TJOIN:
    return std::make_unique<TJoinLite<MaterializeHandler, Filter>>(similarity);
  case PALLOC:
    return std::make_unique<PallocJoin<MaterializeHandler, Filter>>(similarity, shared_state.palloc);
  }
  return std::make_unique<PrefixSignatureJoin<MaterializeHandler, Filter>>(similarity, shared_state.prefix);
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

inline types::Batch dataset_to_batch(types::Dataset& dataset) {
  return std::visit(
    [](auto&& data) {
      using DatasetType = std::decay_t<decltype(data)>;
      return types::Batch(types::DataBatch<typename DatasetType::value_type>(data));
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
                                  similarity::Similarity& similarity,
                                  ontology::QueryPlan& plan,
                                  types::ResultPairs& result_pairs,
                                  int64_t index_offset,
                                  int64_t probe_offset,
                                  IndexedBatch index_batch,
                                  IndexedBatch probe_batch,
                                  ReductionCache reduction_cache,
                                  statistics::LocalJoinStatistics& plan_statistics) {
  for (int32_t level = 1; level < static_cast<int32_t>(plan.steps.size()); ++level) {
    auto reduced_index =
      reduction_cache.reduce_to_level(index_batch, similarity, plan, level, plan_statistics.rc_statistics);
    auto reduced_index_batch = dataset_to_batch(reduced_index->first);
    auto reduced_probe =
      reduction_cache.reduce_to_level(probe_batch, similarity, plan, level, plan_statistics.rc_statistics);
    auto reduced_probe_batch = dataset_to_batch(reduced_probe->first);

    plan_statistics.step_verifications[plan.steps.size() - (level + 1)].add(static_cast<int64_t>(result_pairs.size()));
    offset_verify_with_similarity(
      reduced_index_batch, index_offset, reduced_probe_batch, probe_offset, reduced_index->second, result_pairs);
  }

  // if data was actually reduced, we still have to verify with the "outermost" similarity
  // otherwise, the algorithm instance has already verified this part
  if (!plan.steps.empty()) {
    plan_statistics.step_verifications.back().add(static_cast<int64_t>(result_pairs.size()));
    verify_with_similarity(dataset, similarity, result_pairs);
  }
}

}  // namespace join

#endif  // ALGORITHM_RESOLUTION_HH
