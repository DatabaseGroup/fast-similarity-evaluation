#ifndef SRC_EXECUTION_CACHE_HH
#define SRC_EXECUTION_CACHE_HH

#include <memory>

#include "../ontology/planner.hh"
#include "../similarity/similarity.hh"
#include "../types/types.hh"
#include "../util/lru_cache.hh"

namespace join {
template <class Handler = MaterializeHandler>
struct AlgorithmInstance {
  std::unique_ptr<JoinAlgorithm<Handler>> algorithm{};
  std::vector<std::shared_ptr<types::Dataset>> owned_data{};
  similarity::Similarity similarity;
  bool initialized{false};
};

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
}

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
  std::shared_ptr<types::Dataset> reduce_data_to_level(IndexedBatch& batch,
                                                       ontology::QueryPlan& plan,
                                                       int32_t level,
                                                       statistics::ReductionCacheStatistics& statistics) {
    assert(!plan.steps.empty());
    // find lowest, processed step
    auto batch_id = batch.id;
    auto rit = plan.steps.rbegin() + level;

    std::optional<std::shared_ptr<types::Dataset>> cache_result;
    for (; rit != plan.steps.rend(); ++rit) {
      auto step_id = rit->id;
      cache_result = cache.get({batch_id, step_id});

      if (cache_result.has_value()) {
        break;
      }
    }

    std::shared_ptr<types::Dataset> result;
    if (cache_result.has_value()) {
      statistics.reduction_cache_hits.inc();
      result = *cache_result;
    } else {
      statistics.reduction_cache_misses.inc();
      // we have to start at the top at batch, do the first step manually (due to annoying problems with dataset vs
      // batch) this is why we assume !plan.steps.empty()
      rit = plan.steps.rend();
      --rit;

      auto& reduction_step = *rit;
      auto& reduction = reduction_step.reduction.get();
      auto step_id = reduction_step.id;
      result = cache.emplace({batch_id, step_id}, std::make_shared<types::Dataset>(reduction.reduce_data(batch.batch)));
    }

    // go a step back to highest, unprocessed node
    --rit;

    // we also have to access the first reduction at position 0; hence plan.steps.rbegin() - 1 is the first position to
    // stop
    for (; rit != plan.steps.rbegin() - 1 + level; --rit) {
      auto& reduction_step = *rit;
      auto& reduction = reduction_step.reduction.get();
      auto step_id = reduction_step.id;

      result = cache.emplace({batch_id, step_id}, std::make_shared<types::Dataset>(reduction.reduce_data(*result)));
    }

    return result;
  }

  std::shared_ptr<types::Dataset> reduce_data_to_end(IndexedBatch& batch,
                                                     ontology::QueryPlan& plan,
                                                     statistics::ReductionCacheStatistics& statistics) {
    return reduce_data_to_level(batch, plan, 0, statistics);
  }

  std::vector<std::shared_ptr<types::Dataset>> get_all_reduced_data(IndexedBatch& batch,
                                                     ontology::QueryPlan& plan,
                                                     statistics::ReductionCacheStatistics& statistics) {
    std::vector<std::shared_ptr<types::Dataset>> data;
    data.reserve(plan.steps.size());
    for (int32_t i = 0; i < static_cast<int32_t>(plan.steps.size()); ++i) {
      data.emplace_back(reduce_data_to_level(batch, plan, i, statistics));
    }
    return data;
  }

  std::vector<similarity::Similarity> get_all_reduced_similarities(similarity::Similarity& similarity,
                                                                   ontology::QueryPlan& plan) {
    std::vector<similarity::Similarity> similarities;
    if (!plan.steps.empty()) {
      similarities.resize(plan.steps.size());
      similarities[plan.steps.size() - 1] = plan.steps.front().reduction.get().reduce_similarity(similarity);
      for (size_t i = 1; i < plan.steps.size(); ++i) {
        similarities[plan.steps.size() - i - 1] =
          plan.steps[plan.steps.size() - i].reduction.get().reduce_similarity(similarities.back());
      }
    }
    return similarities;
  }

  similarity::Similarity reduce_similarity_to_end(similarity::Similarity& similarity, ontology::QueryPlan& plan) {
    assert(!plan.steps.empty());
    return std::move(get_all_reduced_similarities(similarity, plan).front());
  }

private:
  // use shared_ptr for pointer stability
  util::LRUCache<CacheHashKey, std::shared_ptr<types::Dataset>> cache;
};

class ProbingSignaturesCache {
public:
  using AlgBatchPair = std::pair<int64_t, size_t>;

  explicit ProbingSignaturesCache(size_t size) : cache(size) {}

  std::shared_ptr<std::any> get_cached_probing_signatures(int64_t plan_id,
                                                          size_t batch_id,
                                                          types::Batch& probe_batch,
                                                          join::JoinAlgorithm<MaterializeHandler>& join_algorithm,
                                                          statistics::ReductionCacheStatistics& statistics) {
    auto cache_key = AlgBatchPair(plan_id, batch_id);

    auto cache_result = cache.get(cache_key);
    if (cache_result.has_value()) {
      statistics.probing_signature_cache_hits.inc();
      return cache_result.value();
    } else {
      statistics.probing_signature_cache_misses.inc();
      auto result =
        cache.emplace(cache_key, std::make_shared<std::any>(join_algorithm.get_probing_signatures(probe_batch)));
      return result;
    }
  }

private:
  util::LRUCache<AlgBatchPair, std::shared_ptr<std::any>> cache;
};

}  // namespace join

#endif  // SRC_EXECUTION_CACHE_HH
