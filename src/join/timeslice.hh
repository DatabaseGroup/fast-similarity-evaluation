#ifndef SRC_TIMESLICE_HH
#define SRC_TIMESLICE_HH

#include "../ontology/uct.hh"
#include "algorithm_resolution.hh"
#include "execution_cache.hh"
#include "../timing/join_timing.hh"

namespace join {

inline void execute_timeslice_prebuilt(data::Dataset& dataset,
                                similarity::Similarity& similarity,
                                std::vector<ontology::QueryPlan>& plans,
                                timing::TimeStaticJoinTiming& timing,
                                std::vector<statistics::LocalJoinStatistics>& all_statistics) {
  ontology::UCT uct = ontology::UCT::from_query_plans(plans);

  std::vector<AlgorithmInstance> algorithms;

  // should be large enough to fit all index data of plans + one microbatch
  // a plan has at most 3 steps and we have ~plans.size + 1 different "batches" at the same time
  ReductionCache reduction_cache(3 * (plans.size() + 1));
  std::vector<IndexedBatch> all_dataset_batches;
  all_dataset_batches.reserve(plans.size());
  for (size_t i = 0; i < plans.size(); ++i) {
    all_dataset_batches.emplace_back(i, types::dataset_to_batch(dataset.data));
  }

  timing.build_time.start();
  for (size_t i = 0; i < plans.size(); ++i) {
    auto& plan = plans[i];
    auto& alg_instance = algorithms.emplace_back();

    alg_instance.initialized = true;
    if (!plan.steps.empty()) {
      alg_instance.owned_data = reduction_cache.reduce_to_end(all_dataset_batches[i], similarity, plan, all_statistics[i]);
    }
    alg_instance.algorithm =
      resolve_algorithmid(plan.algorithm_id, plan.steps.empty() ? similarity : alg_instance.owned_data->second);
    auto index_batch = types::dataset_to_batch(plan.steps.empty() ? dataset.data : alg_instance.owned_data->first);
    alg_instance.algorithm->prepare_indexing_batch(index_batch);
    alg_instance.algorithm->index_batch(index_batch);
  }
  timing.build_time.stop();

  int64_t probing_id = 0;
  const int64_t microbatch = 16;

  std::vector<types::ResultPair> result_pairs;
  MaterializeHandler handler(result_pairs);

  timing.join_time.start();
  while (probing_id < dataset.statistics->count) {
    auto action = uct.select_action();
    auto& selected_plan = plans[action.action];
    auto& alg = algorithms[action.action];
    auto& plan_statistics = all_statistics[action.action];
    plan_statistics.selection_count.inc();

    // execute plan until timeout
    int64_t start_probing_id = probing_id;
    timing::ExecutionCost start_time = timing::start_cost_measurement();
    timing::ExecutionCost end_time;
    double time_required;
    constexpr double TIMESLICE = 0.15;
    while (probing_id < dataset.statistics->count) {
      size_t probing_batch_id = plans.size() + probing_id;
      auto probing_batch = types::get_batch_by_offset(dataset.data, probing_id, probing_id + microbatch);
      auto ipbatch = IndexedBatch(
        probing_batch_id, probing_batch);  // the first ids are used for indexing (should be fixed in the future)

      // todo: clean this up; verification is duplicated from plan_execution.hh
      std::shared_ptr<std::any> null;
      if (selected_plan.steps.empty()) {
        alg.algorithm->join_batch(probing_batch, handler, plan_statistics, null);
      } else {
        auto reduced = reduction_cache.reduce_to_end(ipbatch, similarity, selected_plan, plan_statistics);
        auto reduced_batch = types::dataset_to_batch(reduced->first);

        alg.algorithm->join_batch(reduced_batch, handler, plan_statistics, null);
      }

      for (int32_t level = 1; level < static_cast<int32_t>(selected_plan.steps.size()); ++level) {
        auto reduced_index = reduction_cache.reduce_to_level(
          all_dataset_batches[action.action], similarity, selected_plan, level, plan_statistics);
        auto reduced_index_batch = types::dataset_to_batch(reduced_index->first);
        auto reduced_probe =
          reduction_cache.reduce_to_level(ipbatch, similarity, selected_plan, level, plan_statistics);
        auto reduced_probe_batch = types::dataset_to_batch(reduced_probe->first);

        offset_verify_with_similarity(
          reduced_index_batch, 0, reduced_probe_batch, probing_id, reduced_index->second, result_pairs);
      }

      // if data was actually reduced, we still have to verify with the "outermost" similarity
      if (!selected_plan.steps.empty()) {
        plan_statistics.last_level_verifications.add(static_cast<int64_t>(result_pairs.size()));
        verify_with_similarity(dataset.data, similarity, result_pairs);
      }
      plan_statistics.result_size.add(static_cast<int64_t>(result_pairs.size()));

      result_pairs.clear();
      probing_id += microbatch;
      end_time = timing::end_cost_measurement();
      time_required = timing::get_cost(start_time, end_time);
      if (time_required > TIMESLICE) {
        break;
      }
    }

    int64_t processed_ids = probing_id - start_probing_id;
    double reward =
      static_cast<double>(processed_ids) / static_cast<double>(dataset.statistics->count) / (TIMESLICE / time_required);
    // std::cout << "Update reward: Action " << action.action << " has reward " << reward << std::endl;
    uct.update(action, reward);
  }
  timing.join_time.stop();

  uct.for_each_action([&](ontology::detail::UCTNode& n) {
    all_statistics[n.get_action()].bandit_weights.emplace_back(n.get_mean());
  });
}

}  // namespace join

#endif  // SRC_TIMESLICE_HH
