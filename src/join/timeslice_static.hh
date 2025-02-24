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
                                ontology::QueryPlan& selected_plan,
                                AlgorithmInstance<MaterializeHandler>& alg,
                                types::Batch& probing_batch,
                                int64_t probing_offset,
                                ReductionCache& reduction_cache,
                                MaterializeHandler& handler,
                                statistics::LocalJoinStatistics& plan_statistics) {
  std::shared_ptr<std::any> null;
  FilterConfig config{FilterType::SYMMETRIC_PAIRS};
  if (selected_plan.steps.empty()) {
    auto indexed_data = dataset_to_batch(data);
    alg.algorithm->join_batch(indexed_data, probing_batch, handler, config, plan_statistics, null);
  } else {
    auto reduced = reduction_cache.reduce_data_to_end(ipbatch, selected_plan, plan_statistics.rc_statistics);
    auto reduced_batch = dataset_to_batch(*reduced);
    auto indexed_data = dataset_to_batch(*alg.owned_data.front());

    alg.algorithm->join_batch(indexed_data, reduced_batch, handler, config, plan_statistics, null);
  }

  // this should not be necessary in general, but due to a "bug" in add_small_results, we currently need this
  std::erase_if(handler.results, [](const auto& o) { return o.first >= o.second; });

  verify_pairs_for_plan(data,
                        alg.owned_data,
                        similarity,
                        selected_plan,
                        handler.results,
                        0,
                        probing_offset,
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
                                       double timeslice,
                                       timing::TimeStaticJoinTiming& timing,
                                       std::vector<statistics::LocalTimeSliceStatistics>& all_statistics) {
  for (size_t action = 0; action < plans.size(); ++action) {
    util::print_dbg(absl::StrFormat("Action %i: %s", action, plans[action].to_string()));
  }

  ontology::UCT uct = ontology::UCT::from_query_plans(plans);
  std::vector<AlgorithmInstance<MaterializeHandler>> algorithms;
  std::vector<AlgorithmSharedState<MaterializeHandler>> shared_states(plans.size());

  constexpr int64_t HALFBATCH = 8;
  ReductionCache reduction_cache(2 * (dataset.statistics->count / HALFBATCH + 1));
  std::vector<IndexedBatch> all_dataset_batches;
  all_dataset_batches.reserve(plans.size());
  for (size_t i = 0; i < plans.size(); ++i) {
    all_dataset_batches.emplace_back(i, dataset_to_batch(dataset.data));
  }

  timing.build_time.start();
  for (size_t i = 0; i < plans.size(); ++i) {
    auto& plan = plans[i];
    auto& alg_instance = algorithms.emplace_back();
    auto& shared_state = shared_states[i];
    all_statistics[i].indexed_ratio = 1;

    alg_instance.initialized = true;
    if (!plan.steps.empty()) {
      alg_instance.owned_data.reserve(plan.steps.size());
      for ([[maybe_unused]] auto& _ : plan.steps) {
        alg_instance.owned_data.emplace_back(std::make_shared<types::Dataset>());
      }
      for (int64_t offset = 0; offset < dataset.statistics->count; offset += HALFBATCH) {
        auto batch = get_batch_by_offset(dataset.data, offset, std::min(offset + HALFBATCH, dataset.statistics->count));
        auto iibatch = IndexedBatch(offset, batch);
        auto reduced_batch = reduction_cache.get_all_reduced_data(iibatch, plan, all_statistics[i].rc_statistics);

        for (size_t j = 0; j < reduced_batch.size(); ++j) {
          types::dataset_append(*alg_instance.owned_data[j], *reduced_batch[j]);
        }
      }
      alg_instance.similarity = reduction_cache.reduce_similarity_to_end(similarity, plan);
    }
    alg_instance.algorithm =
      resolve_algorithmid(plan.algorithm_id, plan.steps.empty() ? similarity : alg_instance.similarity, shared_state);
    auto index_batch = dataset_to_batch(plan.steps.empty() ? dataset.data : *alg_instance.owned_data.front());
    alg_instance.algorithm->insert_batch(index_batch, index_batch);
  }
  timing.build_time.stop();

  int64_t lp_id = 0;
  int64_t rp_id = dataset.statistics->count;
  // rp_id is also HALFBATCH aligned
  if (rp_id % HALFBATCH != 0) {
    rp_id = (rp_id / HALFBATCH + 1) * HALFBATCH;
  }
  double scaled_timeslice = timeslice;

  std::vector<types::ResultPair> result_pairs;
  MaterializeHandler handler(result_pairs);

  double total_unweighted_reward = 0;
  double max_reward = 0;
  int64_t iterations = 0;
  int64_t non_punctual = 0;
  int64_t non_punctual_window_size = 0;
  auto next_weight_update = 2 * static_cast<int64_t>(plans.size());
  constexpr int64_t WEIGHT_UPDATE_STEP = 10;

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
        size_t probing_batch_id = lp_id;
        auto probing_batch = get_batch_by_offset(dataset.data, lp_id, std::min(lp_id + HALFBATCH, rp_id));
        auto ipbatch = IndexedBatch(probing_batch_id, probing_batch);

        evaluate_microbatch(dataset.data,
                            similarity,
                            ipbatch,
                            selected_plan,
                            alg,
                            probing_batch,
                            lp_id,
                            reduction_cache,
                            handler,
                            plan_statistics);
        lp_id += HALFBATCH;
      }

      // do right batch
      if (lp_id < rp_id) {
        rp_id = rp_id - HALFBATCH;

        size_t probing_batch_id = rp_id;
        auto probing_batch =
          get_batch_by_offset(dataset.data, rp_id, std::min(rp_id + HALFBATCH, dataset.statistics->count));
        auto ipbatch = IndexedBatch(
          probing_batch_id, probing_batch);  // the first ids are used for indexing (should be fixed in the future)

        evaluate_microbatch(dataset.data,
                            similarity,
                            ipbatch,
                            selected_plan,
                            alg,
                            probing_batch,
                            rp_id,
                            reduction_cache,
                            handler,
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

    double unweighted_reward = static_cast<double>(processed_ids) / static_cast<double>(dataset.statistics->count);
    total_unweighted_reward += unweighted_reward;
    double reward = unweighted_reward / (time_required / timeslice);
    max_reward = std::max(reward, max_reward);
    iterations += 1;
    non_punctual_window_size += 1;

    util::print_dbg(absl::StrFormat(
      "Reward for action %d: %f (Time: %f, #ids: %d)", action.action, reward, time_required, processed_ids));

    if (iterations == next_weight_update) {
      double next_weight = std::max(0., 1. * (1 - 1.5 * total_unweighted_reward)) * max_reward;
      util::print_dbg(absl::StrFormat("Updating UCT weights to %f", next_weight));
      uct.update_exp_weight(next_weight);
      next_weight_update += WEIGHT_UPDATE_STEP;
      if (static_cast<double>(non_punctual) / static_cast<double>(non_punctual_window_size) > 0.25) {
        non_punctual = 0;
        non_punctual_window_size = 0;
        scaled_timeslice *= 2;
      }
    }

    uct.update(action, reward, time_required / timeslice);
  }
  timing.join_time.stop();

  uct.for_each_action(
    [&](ontology::detail::UCTNode& n) { all_statistics[n.get_action()].bandit_weight.record(n.get_mean()); });
}

}  // namespace join

#endif  // SRC_TIMESLICE_STATIC_HH
