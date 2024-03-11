#ifndef SRC_PLAN_EXECUTION_HH
#define SRC_PLAN_EXECUTION_HH

#include <algorithm>

#include "../ontology/bandit.hh"
#include "../ontology/planner.hh"
#include "../similarity/similarity.hh"
#include "../statistics/join_statistics.hh"
#include "../timing/cycles.hh"
#include "../types/types.hh"
#include "../util/lru_cache.hh"
#include "../util/visit_overload.hh"
#include "result_handler.hh"
#include "signature_join.hh"

namespace join {

template <class Handler>
std::unique_ptr<JoinAlgorithm<Handler>> resolve_algorithmid(AlgorithmId id, similarity::Similarity& similarity) {
  switch (id) {
  case PREFIX_SIGNATURE_JOIN:
    return std::make_unique<PrefixSignatureJoin<Handler>>(similarity);
  case FALLBACK:
    // todo implement comparing all pairs as obvious fallback
    break;
  case PASS_JOIN:
    return std::make_unique<PassJoin<Handler>>(similarity);
    break;
  }
  return std::make_unique<PrefixSignatureJoin<Handler>>(similarity);
}

template <class DataType, class SimilarityPtr>
void _verify(DataType& dataset, SimilarityPtr& similarity, std::vector<types::ResultPair>& pairs) {
  pairs.erase(std::remove_if(pairs.begin(),
                             pairs.end(),
                             [&](auto& pair) {
                               auto l_id = pair.first;
                               auto r_id = pair.second;
                               auto& l = dataset[l_id];
                               auto& r = dataset[r_id];
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
                               auto& l = left_dataset[l_id - left_offset];
                               auto& r = right_dataset[r_id - right_offset];
                               return !similarity->is_in_threshold(l, r);
                             }),
              pairs.end());
}

void verify_with_similarity(types::Dataset& dataset,
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

void offset_verify_with_similarity(types::Batch& left_dataset,
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

int64_t get_offset_into_batch(int64_t batch_idx, int64_t batch_size) { return batch_idx * batch_size; }

struct IndexedBatch {
  const size_t id;
  types::Batch batch;

  IndexedBatch(size_t id, types::Batch batch) : id(id), batch(batch) {}
};

struct CacheHashKey {
  size_t batch_id;
  size_t reduction_id;

  template <typename H>
  friend H AbslHashValue(H h, const CacheHashKey& k) {
    return H::combine(std::move(h), k.batch_id, k.reduction_id);
  }

  bool operator==(const CacheHashKey& rhs) const {
    return batch_id == rhs.batch_id && reduction_id == rhs.reduction_id;
  }

  CacheHashKey(size_t batchId, size_t reductionId) : batch_id(batchId), reduction_id(reductionId) {}
};

}  // namespace join

// make CacheHashKey also hashable with std::unordered_map (used for debugging, because absl::flat_hash_map is ugly)
template <>
struct std::hash<join::CacheHashKey> {
  std::size_t operator()(join::CacheHashKey const& n) const noexcept {
    size_t hash = 0;
    boost::hash_combine(hash, n.batch_id);
    boost::hash_combine(hash, n.reduction_id);
    return hash;
  }
};

namespace join {

class ReductionCache {
public:
  explicit ReductionCache(size_t cache_size) : cache(cache_size) {}

public:
  std::shared_ptr<std::pair<types::Dataset, similarity::Similarity>>
  reduce_to_level(IndexedBatch& batch, similarity::Similarity& similarity, ontology::QueryPlan& plan, int32_t level) {
    assert(!plan.steps.empty());
    // find lowest, processed step
    auto batch_id = batch.id;
    auto rit = plan.steps.rbegin() + level;

    std::optional<std::shared_ptr<std::pair<types::Dataset, similarity::Similarity>>> cache_result;
    for (; rit != plan.steps.rend(); ++rit) {
      auto step_id = rit->id;
      cache_result = cache.get({batch_id, step_id});

      if (cache_result.has_value()) {
        break;
      }
    }

    std::shared_ptr<std::pair<types::Dataset, similarity::Similarity>> result_pair;
    if (cache_result.has_value()) {
      result_pair = *cache_result;
    } else {
      // we have to start at the top at batch, do the first step manually (due to annoying problems with dataset vs
      // batch) this is why we assume !plan.steps.empty()
      rit = plan.steps.rend();
      --rit;

      auto& reduction_step = *rit;
      auto& reduction = reduction_step.reduction.get();
      auto step_id = reduction_step.id;
      result_pair = cache.emplace({batch_id, step_id},
                                  std::make_shared<std::pair<types::Dataset, similarity::Similarity>>(
                                    reduction.reduce_data(batch.batch), reduction.reduce_similarity(similarity)));
    }

    // go a step back to highest, unprocessed node
    --rit;

    // we also have to access the first reduction at position 0; hence plan.steps.rbegin() - 1 is the first position to
    // stop
    for (; rit != plan.steps.rbegin() - 1 + level; --rit) {
      auto& reduction_step = *rit;
      auto& reduction = reduction_step.reduction.get();
      auto step_id = reduction_step.id;

      result_pair =
        cache.emplace({batch_id, step_id},
                      std::make_shared<std::pair<types::Dataset, similarity::Similarity>>(
                        reduction.reduce_data(result_pair->first), reduction.reduce_similarity(result_pair->second)));
    }

    return result_pair;
  }

  std::shared_ptr<std::pair<types::Dataset, similarity::Similarity>> reduce_to_end(IndexedBatch& batch,
                                                                                   similarity::Similarity& similarity,
                                                                                   ontology::QueryPlan& plan) {
    return reduce_to_level(batch, similarity, plan, 0);
  }

private:
  // use shared_ptr for pointer stability
  util::LRUCache<CacheHashKey, std::shared_ptr<std::pair<types::Dataset, similarity::Similarity>>> cache;
};

class AlgorithmCache {
private:
  struct AlgorithmInstance {
    std::unique_ptr<join::JoinAlgorithm<MaterializeHandler>> algorithm{};
    std::shared_ptr<std::pair<types::Dataset, similarity::Similarity>> owned_data{};
    bool initialized{false};
  };
public:
  AlgorithmCache(IndexedBatch& index_batch, ReductionCache& reductionCache, size_t algorithm_count)
      : index_batch(index_batch), reduction_cache(reductionCache), algorithms(algorithm_count) {}

public:
  void probe_using_plan(IndexedBatch& probe_batch,
                        similarity::Similarity& similarity,
                        int64_t plan_idx,
                        std::vector<ontology::QueryPlan>& plans,
                        MaterializeHandler& handler,
                        statistics::LocalJoinStatistics& statistics) {
    auto& plan = plans[plan_idx];

    auto& alg_instance = algorithms[plan_idx];
    if (!alg_instance.initialized) {
      // if reductions are necessary
      if (plan.steps.empty()) {
        // the dataset and similarity are owned by the caller
        alg_instance.algorithm = resolve_algorithmid<MaterializeHandler>(plan.algorithm_id, similarity);
        alg_instance.algorithm->prepare_indexing_batch(index_batch.batch);
        alg_instance.algorithm->index_batch(index_batch.batch);
      } else {
        // the dataset and similarity are owned by the AlgorithmInstance
        auto reduced = reduction_cache.reduce_to_end(index_batch, similarity, plan);
        alg_instance.owned_data = reduced;
        alg_instance.algorithm = resolve_algorithmid<MaterializeHandler>(plan.algorithm_id, alg_instance.owned_data->second);

        auto batch = types::dataset_to_batch(alg_instance.owned_data->first);
        alg_instance.algorithm->prepare_indexing_batch(batch);
        alg_instance.algorithm->index_batch(batch);
      }

      alg_instance.initialized = true;
    }

    if (plan.steps.empty()) {
      // the dataset and similarity are owned by the caller
      if (index_batch.id == probe_batch.id) {
        alg_instance.algorithm->selfjoin_batch(probe_batch.batch, handler, statistics);
      } else {
        alg_instance.algorithm->join_batch(probe_batch.batch, handler, statistics);
      }
    } else {
      // reduce first, this function is temporary owner of the data
      auto reduced_probe = reduction_cache.reduce_to_end(probe_batch, similarity, plan);
      auto batch = types::dataset_to_batch(reduced_probe->first);
      if (index_batch.id == probe_batch.id) {
        alg_instance.algorithm->selfjoin_batch(batch, handler, statistics);
      } else {
        alg_instance.algorithm->join_batch(batch, handler, statistics);
      }
    }
  }

private:
  IndexedBatch& index_batch;
  ReductionCache& reduction_cache;
  std::vector<AlgorithmInstance> algorithms;
};

class PlanExecutor {
public:
  explicit PlanExecutor(int64_t blockSize, size_t cache_size) : block_size(blockSize), reduction_cache(cache_size) {}

public:
  void execute_plans(data::Dataset& dataset,
                     similarity::Similarity& similarity,
                     std::vector<ontology::QueryPlan>& plans,
                     std::vector<statistics::LocalJoinStatistics>& all_statistics) {
    int64_t batch_count = get_batch_count(dataset.statistics->count);
    int64_t all_batch_pairs = get_allpairs_batches(batch_count);

    ontology::Exp3LightA bandit(static_cast<int64_t>(plans.size()), all_batch_pairs);

    for (int64_t index_batch_idx = 0; index_batch_idx < batch_count; ++index_batch_idx) {
      auto index_batch = IndexedBatch(index_batch_idx, types::get_batch(dataset.data, index_batch_idx, block_size));
      auto index_offset = get_offset_into_batch(index_batch_idx, block_size);

      AlgorithmCache algorithm_cache(index_batch, reduction_cache, plans.size());

      for (int64_t probe_batch_idx = index_batch_idx; probe_batch_idx < batch_count; ++probe_batch_idx) {
        auto probe_batch = IndexedBatch(probe_batch_idx, types::get_batch(dataset.data, probe_batch_idx, block_size));
        auto probe_offset = get_offset_into_batch(probe_batch_idx, block_size);

        int64_t plan_id = bandit.select_arm();
        timing::ticks start_ticks = timing::cpu_cycles_start();

        auto& plan = plans[plan_id];
        auto& plan_statistics = all_statistics[plan_id];
        plan_statistics.selection_count.inc();

        types::ResultPairs result_pairs;
        MaterializeHandler handler(result_pairs);

        algorithm_cache.probe_using_plan(probe_batch, similarity, plan_id, plans, handler, plan_statistics);

        for (int32_t level = 1; level < static_cast<int32_t>(plan.steps.size()); ++level) {
          auto reduced_index =
            reduction_cache.reduce_to_level(index_batch, similarity, plan, level);
          auto reduced_index_batch = types::dataset_to_batch(reduced_index->first);
          auto reduced_probe =
            reduction_cache.reduce_to_level(probe_batch, similarity, plan, level);
          auto reduced_probe_batch = types::dataset_to_batch(reduced_probe->first);

          plan_statistics.filter_verifications.add(static_cast<int64_t>(result_pairs.size()));
          offset_verify_with_similarity(
            reduced_index_batch, index_offset, reduced_probe_batch, probe_offset, reduced_index->second, result_pairs);
        }

        // if data was actually reduced, we still have to verify with the "outermost" similarity
        if (!plan.steps.empty()) {
          plan_statistics.filter_verifications.add(static_cast<int64_t>(result_pairs.size()));
          verify_with_similarity(dataset.data, similarity, result_pairs);
        }

        timing::ticks end_ticks = timing::cpu_cycles_start();
        auto loss = static_cast<long double>(end_ticks - start_ticks);
        bandit.update_weights(plan_id, loss);
        plan_statistics.result_size.add(static_cast<int64_t>(result_pairs.size()));

        // types::print_result_pairs(std::cout, result_pairs, dataset.data);
      }
    }

    for (int32_t i = 0; i < static_cast<int32_t>(plans.size()); ++i) {
      all_statistics[i].bandit_weight = bandit.get_normalized_weight(i);
    }
  }

private:
  int64_t get_batch_count(size_t input_size) {
    auto res = static_cast<int64_t>(input_size) / block_size;
    if (static_cast<int64_t>(input_size) % block_size != 0) {
      ++res;
    }
    return res;
  }

  int64_t get_allpairs_batches(int64_t batch_count) { return (batch_count * (batch_count - 1)) / 2; }

private:
  int64_t block_size;
  ReductionCache reduction_cache;
};

}  // namespace join

#endif  // SRC_PLAN_EXECUTION_HH
