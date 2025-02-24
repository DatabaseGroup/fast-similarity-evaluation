#ifndef SRC_PLAN_EXECUTION_HH
#define SRC_PLAN_EXECUTION_HH

#include <algorithm>

#include "../data/dataset.hh"
#include "../ontology/bandit.hh"
#include "../ontology/planner.hh"
#include "../similarity/similarity.hh"
#include "../statistics/join_statistics.hh"
#include "../timing/cost_measurement.hh"
#include "../types/types.hh"
#include "algorithm_building_blocks.hh"
#include "execution_cache.hh"

namespace join {

struct CostPair {
  timing::ExecutionCost start;
  timing::ExecutionCost end;

  [[nodiscard]] timing::cost_type get_cost() const { return timing::get_cost(start, end); }
};

struct BatchCost {
  CostPair indexing;
  CostPair probing_preprocessing;
  CostPair candidate_generation;
  CostPair verification;

  [[nodiscard]] timing::cost_type get_cost() const {
    timing::cost_type cost{};
    cost += indexing.get_cost();
    cost += probing_preprocessing.get_cost();
    cost += candidate_generation.get_cost();
    cost += verification.get_cost();

    return cost;
  }
};

// this implicitly caches indexing signatures
class AlgorithmCache {
public:
  AlgorithmCache(IndexedBatch& index_batch,
                 ReductionCache& reductionCache,
                 ProbingSignaturesCache& probing_signatures_cache,
                 size_t algorithm_count)
      : index_batch(index_batch),
        reduction_cache(reductionCache),
        probing_signatures_cache(probing_signatures_cache),
        algorithms(algorithm_count),
        shared_states(algorithm_count) {}

public:
  void probe_using_plan(IndexedBatch& probe_batch,
                        similarity::Similarity& similarity,
                        int64_t plan_idx,
                        std::vector<ontology::QueryPlan>& plans,
                        MaterializeHandler& handler,
                        BatchCost& batch_cost,
                        statistics::LocalJoinStatistics& statistics) {
    auto& plan = plans[plan_idx];
    auto& shared_state = shared_states[plan_idx];

    batch_cost.indexing.start = timing::start_cost_measurement();
    auto& alg_instance = algorithms[plan_idx];
    if (!alg_instance.initialized) {
      // if reductions are necessary
      if (plan.steps.empty()) {
        // the dataset and similarity are owned by the caller
        alg_instance.algorithm = resolve_algorithmid(plan.algorithm_id, similarity, shared_state);
        alg_instance.algorithm->insert_batch(index_batch.batch, index_batch.batch);
      } else {
        // the dataset and similarity are owned by the AlgorithmInstance
        auto reduced = reduction_cache.get_all_reduced_data(index_batch, plan, statistics.rc_statistics);
        alg_instance.owned_data = reduced;
        alg_instance.similarity = reduction_cache.reduce_similarity_to_end(similarity, plan);
        alg_instance.algorithm = resolve_algorithmid(plan.algorithm_id, alg_instance.similarity, shared_state);

        auto batch = dataset_to_batch(*alg_instance.owned_data.front());
        alg_instance.algorithm->insert_batch(batch, batch);
      }

      alg_instance.initialized = true;
    }
    batch_cost.indexing.end = timing::end_cost_measurement();

    std::shared_ptr<std::any> cached_probing_signatures;
    // do join
    if (plan.steps.empty()) {
      batch_cost.probing_preprocessing.start = timing::start_cost_measurement();
      // the dataset and similarity are owned by the caller
      if (alg_instance.algorithm->has_independent_probing_signatures()) {
        cached_probing_signatures = probing_signatures_cache.get_cached_probing_signatures(
          plan_idx, probe_batch.id, probe_batch.batch, *alg_instance.algorithm, statistics.rc_statistics);
      }
      batch_cost.probing_preprocessing.end = timing::end_cost_measurement();

      batch_cost.candidate_generation.start = timing::start_cost_measurement();
      FilterConfig config{};
      if (index_batch.id == probe_batch.id) {
        config.type = FilterType::SIMPLE_SELFJOIN;
      } else {
        config.type = FilterType::NOP;
      }
      alg_instance.algorithm->join_batch(
        index_batch.batch, probe_batch.batch, handler, config, statistics, cached_probing_signatures);
      batch_cost.candidate_generation.end = timing::end_cost_measurement();
    } else {
      batch_cost.probing_preprocessing.start = timing::start_cost_measurement();
      // reduce first, this function is temporary owner of the data
      auto reduced_probe = reduction_cache.reduce_data_to_end(probe_batch, plan, statistics.rc_statistics);
      auto batch = dataset_to_batch(*reduced_probe);
      auto indexed_data = dataset_to_batch(*alg_instance.owned_data.front());

      if (alg_instance.algorithm->has_independent_probing_signatures()) {
        cached_probing_signatures = probing_signatures_cache.get_cached_probing_signatures(
          plan_idx, probe_batch.id, batch, *alg_instance.algorithm, statistics.rc_statistics);
      }
      batch_cost.probing_preprocessing.end = timing::end_cost_measurement();

      batch_cost.candidate_generation.start = timing::start_cost_measurement();
      FilterConfig config{};
      if (index_batch.id == probe_batch.id) {
        config.type = FilterType::SIMPLE_SELFJOIN;
      } else {
        config.type = FilterType::NOP;
      }
      alg_instance.algorithm->join_batch(indexed_data, batch, handler, config, statistics, cached_probing_signatures);
      batch_cost.candidate_generation.end = timing::end_cost_measurement();
    }
  }

  AlgorithmInstance<>& get_algorithm_instance(int64_t plan_idx) { return algorithms[plan_idx]; }

private:
  IndexedBatch& index_batch;
  ReductionCache& reduction_cache;
  ProbingSignaturesCache& probing_signatures_cache;
  std::vector<AlgorithmInstance<>> algorithms;
  std::vector<AlgorithmSharedState<MaterializeHandler>> shared_states;
};

class PlanExecutor {
public:
  explicit PlanExecutor(int64_t batch_count, size_t reduction_cache_size, size_t probing_cache_size)
      : batch_count(batch_count), reduction_cache(reduction_cache_size), probing_signatures_cache(probing_cache_size) {}

public:
  void execute_plans(data::Dataset& dataset,
                     similarity::Similarity& similarity,
                     std::vector<ontology::QueryPlan>& plans,
                     std::vector<statistics::LocalBlockSliceStatistics>& all_statistics) {
    const int64_t all_batch_pairs = get_allpairs_batches(batch_count);
    const int64_t batch_size = get_batch_size(batch_count, dataset.statistics->count);
    int64_t remaining_all_batch_pairs = all_batch_pairs;

    ontology::Exp3LightA bandit(static_cast<int64_t>(plans.size()), all_batch_pairs);

    for (int64_t index_batch_idx = 0; index_batch_idx < batch_count; ++index_batch_idx) {
      auto index_batch = IndexedBatch(index_batch_idx, get_batch_by_id(dataset.data, index_batch_idx, batch_size));
      auto index_offset = get_offset_into_batch(index_batch_idx, batch_size);

      AlgorithmCache algorithm_cache(index_batch, reduction_cache, probing_signatures_cache, plans.size());

      // even rounds are left-to-right, odd right-to-left
      bool left_to_right_direction = (index_batch_idx & INT64_C(1)) == 0;

      for (int64_t i = index_batch_idx; i < batch_count; ++i) {
        int64_t probe_batch_idx;
        if (left_to_right_direction) {
          probe_batch_idx = i;
        } else {
          probe_batch_idx = batch_count + index_batch_idx - i - 1;
        }

        auto probe_batch = IndexedBatch(probe_batch_idx, get_batch_by_id(dataset.data, probe_batch_idx, batch_size));
        auto probe_offset = get_offset_into_batch(probe_batch_idx, batch_size);

        int64_t plan_id = bandit.select_arm();
        BatchCost batch_cost;

        auto& plan = plans[plan_id];
        auto& plan_statistics = all_statistics[plan_id];
        plan_statistics.selection_count.inc();

        types::ResultPairs result_pairs;
        MaterializeHandler handler(result_pairs);

        algorithm_cache.probe_using_plan(probe_batch, similarity, plan_id, plans, handler, batch_cost, plan_statistics);

        batch_cost.verification.start = timing::start_cost_measurement();
        verify_pairs_for_plan(dataset.data,
                              algorithm_cache.get_algorithm_instance(plan_id).owned_data,
                              similarity,
                              plan,
                              result_pairs,
                              index_offset,
                              probe_offset,
                              probe_batch,
                              reduction_cache,
                              plan_statistics);
        batch_cost.verification.end = timing::end_cost_measurement();

        auto loss = static_cast<long double>(batch_cost.get_cost());
        bandit.update_weights(plan_id, loss);
        plan_statistics.incurred_loss += loss;
        plan_statistics.result_size.add(static_cast<int64_t>(result_pairs.size()));

        // types::print_result_pairs(std::cerr, result_pairs, dataset.data);

        /* somewhat useful for debugging the bandit
        std::cerr << "Algorithm " << plan.to_string() << "\n\tloss:" << loss << std::endl;
        std::cerr << "\tweights:" << std::endl;
        for (int32_t j = 0; j < static_cast<int32_t>(plans.size()); ++j) {
          std::cerr << "\t\t" << plans[j].to_string() << ": " << bandit.get_normalized_weight(j) << std::endl;
        }
         */

        --remaining_all_batch_pairs;
        if (all_batch_pairs > 10 && remaining_all_batch_pairs % (all_batch_pairs / 10) == 0) {
          for (int32_t j = 0; j < static_cast<int32_t>(plans.size()); ++j) {
            all_statistics[j].bandit_weight.record(bandit.get_normalized_weight(j));
          }
        }
      }
    }

    for (int32_t j = 0; j < static_cast<int32_t>(plans.size()); ++j) {
      all_statistics[j].expected_total_loss = bandit.get_expected_total_loss(j);
    }
  }

  static int64_t get_batch_size(int64_t batch_count, int64_t input_size) {
    int64_t size = input_size / batch_count + ((input_size % batch_count) != 0);
    return size;
  }
  static int64_t get_allpairs_batches(int64_t batch_count) { return (batch_count * (batch_count - 1)) / 2; }

private:
  int64_t batch_count;
  ReductionCache reduction_cache;
  ProbingSignaturesCache probing_signatures_cache;
};

}  // namespace join

#endif  // SRC_PLAN_EXECUTION_HH
