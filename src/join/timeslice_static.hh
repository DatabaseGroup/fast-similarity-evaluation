#ifndef SRC_TIMESLICE_STATIC_HH
#define SRC_TIMESLICE_STATIC_HH

#include "../ontology/uct.hh"
#include "../timing/cost_measurement.hh"
#include "../timing/join_timing.hh"
#include "../util/debug.hh"
#include "algorithm_building_blocks.hh"
#include "execution_cache.hh"

namespace join {

inline void evaluate_microbatch(types::Dataset& data,
                                similarity::Similarity& similarity,
                                IndexedBatch& ipbatch,
                                ontology::UCT::Selection& action,
                                ontology::QueryPlan& selected_plan,
                                AlgorithmInstance<MaterializeHandler>& alg,
                                types::Batch& probing_batch,
                                int64_t probing_offset,
                                ReductionCache& reduction_cache,
                                MaterializeHandler& handler,
                                std::vector<IndexedBatch>& all_dataset_batches,
                                statistics::LocalJoinStatistics& plan_statistics) {
  std::shared_ptr<std::any> null;
  FilterConfig config{FilterType::SYMMETRIC_PAIRS};
  if (selected_plan.steps.empty()) {
    alg.algorithm->join_batch(probing_batch, handler, config, plan_statistics, null);
  } else {
    auto reduced = reduction_cache.reduce_to_end(ipbatch, similarity, selected_plan, plan_statistics.rc_statistics);
    auto reduced_batch = dataset_to_batch(reduced->first);

    alg.algorithm->join_batch(reduced_batch, handler, config, plan_statistics, null);
  }

  // this should not be necessary in general, but due to a "bug" in add_small_results, we currently need this
  std::erase_if(handler.results, [](const auto& o) { return o.first >= o.second; });

  verify_pairs_for_plan(data,
                        similarity,
                        selected_plan,
                        handler.results,
                        0,
                        probing_offset,
                        all_dataset_batches[action.action],
                        ipbatch,
                        reduction_cache,
                        plan_statistics);
  // types::print_result_pairs(std::cerr, handler.results, data);
  plan_statistics.result_size.add(static_cast<int64_t>(handler.results.size()));
  handler.results.clear();
}

inline void execute_timeslice_prebuilt(data::Dataset& dataset,
                                       similarity::Similarity& similarity,
                                       std::vector<ontology::QueryPlan>& plans,
                                       timing::TimeStaticJoinTiming& timing,
                                       std::vector<statistics::LocalTimeSliceStatistics>& all_statistics) {
  ontology::UCT uct = ontology::UCT::from_query_plans(plans);
  std::vector<AlgorithmInstance<MaterializeHandler>> algorithms;
  AlgorithmSharedState<MaterializeHandler> shared_state;

  // should be large enough to fit all index data of plans + one microbatch
  // a plan has at most 3 steps and we have ~plans.size + 1 different "batches" at the same time
  ReductionCache reduction_cache(3 * (plans.size() + 1));
  std::vector<IndexedBatch> all_dataset_batches;
  all_dataset_batches.reserve(plans.size());
  for (size_t i = 0; i < plans.size(); ++i) {
    all_dataset_batches.emplace_back(i, dataset_to_batch(dataset.data));
  }

  timing.build_time.start();
  for (size_t i = 0; i < plans.size(); ++i) {
    auto& plan = plans[i];
    auto& alg_instance = algorithms.emplace_back();

    alg_instance.initialized = true;
    if (!plan.steps.empty()) {
      alg_instance.owned_data =
        reduction_cache.reduce_to_end(all_dataset_batches[i], similarity, plan, all_statistics[i].rc_statistics);
    }
    alg_instance.algorithm = resolve_algorithmid(
      plan.algorithm_id, plan.steps.empty() ? similarity : alg_instance.owned_data->second, shared_state);
    auto index_batch = dataset_to_batch(plan.steps.empty() ? dataset.data : alg_instance.owned_data->first);
    alg_instance.algorithm->insert_batch(index_batch);
  }
  timing.build_time.stop();

  int64_t lp_id = 0;
  int64_t rp_id = dataset.statistics->count;
  constexpr int64_t HALFBATCH = 8;
  constexpr double TIMESLICE = 0.15;
  double scaled_timeslice = TIMESLICE;

  std::vector<types::ResultPair> result_pairs;
  MaterializeHandler handler(result_pairs);

  double total_reward = 0;
  int64_t iterations = 0;
  int64_t non_punctual = 0;
  auto next_weight_update = static_cast<int64_t>(plans.size());

  timing.join_time.start();
  while (lp_id < rp_id) {
    auto action = uct.select_action();
    auto& selected_plan = plans[action.action];
    auto& alg = algorithms[action.action];
    auto& plan_statistics = all_statistics[action.action];
    plan_statistics.selection_count.inc();

    // execute plan until timeout
    timing::ExecutionCost start_time = timing::start_cost_measurement();
    timing::ExecutionCost end_time;
    double time_required;
    int64_t processed_ids = 0;
    while (lp_id < rp_id) {
      // do left batch
      {
        size_t probing_batch_id = plans.size() + lp_id;
        auto probing_batch = get_batch_by_offset(dataset.data, lp_id, lp_id + HALFBATCH);
        auto ipbatch = IndexedBatch(
          probing_batch_id, probing_batch);  // the first ids are used for indexing (might be fixed in the future)

        evaluate_microbatch(dataset.data,
                            similarity,
                            ipbatch,
                            action,
                            selected_plan,
                            alg,
                            probing_batch,
                            lp_id,
                            reduction_cache,
                            handler,
                            all_dataset_batches,
                            plan_statistics);
        lp_id += HALFBATCH;
      }

      // do right batch
      if (lp_id < rp_id) {
        int64_t tmp = rp_id;
        rp_id = rp_id - HALFBATCH < lp_id ? lp_id : rp_id - HALFBATCH;
        int64_t real_batch = tmp - rp_id;

        size_t probing_batch_id = plans.size() + rp_id;
        auto probing_batch = get_batch_by_offset(dataset.data, rp_id, rp_id + real_batch);
        auto ipbatch = IndexedBatch(
          probing_batch_id, probing_batch);  // the first ids are used for indexing (should be fixed in the future)

        evaluate_microbatch(dataset.data,
                            similarity,
                            ipbatch,
                            action,
                            selected_plan,
                            alg,
                            probing_batch,
                            rp_id,
                            reduction_cache,
                            handler,
                            all_dataset_batches,
                            plan_statistics);
      }

      processed_ids += 2 * HALFBATCH;
      end_time = timing::end_cost_measurement();
      time_required = timing::get_cost(start_time, end_time);
      if (time_required > scaled_timeslice) {
        if (time_required / scaled_timeslice > 1.25) {
          non_punctual++;
        }

        break;
      }
    }

    double reward =
      static_cast<double>(processed_ids) / static_cast<double>(dataset.statistics->count) / (time_required / TIMESLICE);
    total_reward += reward;
    iterations += 1;

    util::print_dbg(absl::StrFormat(
      "Reward for action %d: %f (Time: %f, #ids: %d)", action.action, reward, time_required, processed_ids));

    if (iterations == next_weight_update) {
      double avg_reward = total_reward / static_cast<double>(iterations);
      double next_weight = std::exp2(std::floor(std::log2(avg_reward)));
      uct.update_exp_weight(next_weight);
      next_weight_update *= 2;

      if (static_cast<double>(non_punctual) / static_cast<double>(iterations) > 0.25) {
        scaled_timeslice *= 2;
      }
    }

    uct.update(action, reward);
  }
  timing.join_time.stop();

  uct.for_each_action(
    [&](ontology::detail::UCTNode& n) { all_statistics[n.get_action()].bandit_weight.record(n.get_mean()); });
}

}  // namespace join

#endif  // SRC_TIMESLICE_STATIC_HH
