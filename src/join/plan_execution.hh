#ifndef SRC_PLAN_EXECUTION_HH
#define SRC_PLAN_EXECUTION_HH

#include <algorithm>

#include "../ontology/planner.hh"
#include "../similarity/similarity.hh"
#include "../types/types.hh"
#include "../util/visit_overload.hh"
#include "result_handler.hh"
#include "signature_join.hh"
#include "../ontology/bandit.hh"
#include "../timing/cycles.hh"

namespace join {

template <class Handler>
std::unique_ptr<JoinAlgorithm<Handler>> resolve_algorithmid(AlgorithmId id, similarity::Similarity& similarity) {
  switch (id) {
  case PREFIX_SIGNATURE_JOIN:
    return std::make_unique<PrefixSignatureJoin<Handler>>(similarity);
  case FALLBACK:
    // todo implement comparing all pairs as obvious fallback
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

void offset_verify_with_similarity(types::Dataset& left_dataset,
                                   int64_t left_offset,
                                   types::Dataset& right_dataset,
                                   int64_t right_offset,
                                   similarity::Similarity& similarity,
                                   std::vector<types::ResultPair>& pairs) {
  auto set_verify = [&](types::Sets& left_sets) {
    _offset_verify(left_sets,
                   left_offset,
                   std::get<types::Sets>(right_dataset),
                   right_offset,
                   std::get<similarity::SetSimilarityPtr>(similarity),
                   pairs);
  };
  auto string_verify = [&](types::Strings& left_strings) {
    _offset_verify(left_strings,
                   left_offset,
                   std::get<types::Strings>(right_dataset),
                   right_offset,
                   std::get<similarity::StringSimilarityPtr>(similarity),
                   pairs);
  };
  auto tree_verify = [&](types::Trees& left_trees) {
    _offset_verify(left_trees,
                   left_offset,
                   std::get<types::Trees>(right_dataset),
                   right_offset,
                   std::get<similarity::TreeSimilarityPtr>(similarity),
                   pairs);
  };

  std::visit(util::overloaded{set_verify, string_verify, tree_verify}, left_dataset);
}

static const int64_t BATCH_SIZE = 10000;

int64_t get_offset_into_batch(int64_t batch_idx, int64_t batch_size) { return batch_idx * batch_size; }

void interleave_plans(data::Dataset& dataset,
                      similarity::Similarity& similarity,
                      std::vector<ontology::QueryPlan>& plans) {
  int64_t batch_count = static_cast<int64_t>(dataset.statistics->count) / BATCH_SIZE;
  if (dataset.statistics->count % BATCH_SIZE != 0) {
    ++batch_count;
  }
  int64_t all_batch_pairs = (batch_count * (batch_count - 1)) / 2;
  ontology::Exp3LightA bandit(static_cast<int64_t>(plans.size()), all_batch_pairs);

  for (int64_t index_batch_idx = 0; index_batch_idx < batch_count; ++index_batch_idx) {
    auto index_batch = types::get_batch(dataset.data, index_batch_idx, BATCH_SIZE);
    auto index_offset = get_offset_into_batch(index_batch_idx, BATCH_SIZE);
    std::vector<std::unique_ptr<join::JoinAlgorithm<MaterializeHandler>>> algorithms(plans.size());

    for (int64_t probe_batch_idx = index_batch_idx; probe_batch_idx < batch_count; ++probe_batch_idx) {
      auto probe_batch = types::get_batch(dataset.data, probe_batch_idx, BATCH_SIZE);
      auto probe_offset = get_offset_into_batch(probe_batch_idx, BATCH_SIZE);

      // todo select plan using bandit
      int64_t plan_id = bandit.select_arm();
      timing::ticks start_ticks = timing::cpu_cycles_start();

      auto& plan = plans[plan_id];

      // perform reduction of index data + indexing
      auto last_index_batch = index_batch;
      std::reference_wrapper<similarity::Similarity> last_similarity = similarity;
      for (auto& step : plan.steps) {
        auto& reduction = step.first.get();
        auto& state = step.second.get();

        if (state.prepared_index_batch != index_batch_idx) {
          state.intermediate_data = std::move(reduction.reduce_data(last_index_batch));
          state.intermediate_similarity = std::move(reduction.reduce_similarity(last_similarity));

          state.prepared_index_batch = index_batch_idx;
        }
        last_index_batch = types::dataset_to_batch(state.intermediate_data);
        last_similarity = state.intermediate_similarity;
      }

      if (plan.query_state.algorithm_prepared != index_batch_idx) {
        algorithms[plan_id] = resolve_algorithmid<MaterializeHandler>(plan.algorithm_id, last_similarity);
        auto& algorithm = algorithms[plan_id];

        algorithm->prepare_indexing_batch(last_index_batch);
        algorithm->index_batch(last_index_batch);
      }

      // perform reduction of probing data + probing + verification
      auto last_probe_batch = probe_batch;
      std::vector<types::Dataset> intermediate_probe_data;
      for (auto& step : plan.steps) {
        auto& reduction = step.first.get();

        intermediate_probe_data.emplace_back(std::move(reduction.reduce_data(last_probe_batch)));
        last_probe_batch = types::dataset_to_batch(intermediate_probe_data.back());
      }

      auto& algorithm = algorithms[plan_id];
      algorithm->prepare_probing_batch(last_probe_batch);
      std::vector<types::ResultPair> result_pairs;
      MaterializeHandler handler(result_pairs);
      algorithm->join_batch(last_probe_batch, handler);

      // verification is done from lowest to highest reduction level
      // the lowest level at size() - 1 is part of the algorithm step as the algorithm might
      // be able to optimize verification depending on its specifics
      // hence, we can skip verification for size() - 1 and start at size() - 2
      auto i = static_cast<int64_t>(plan.steps.size()) - 2;
      while (0 <= i) {
        auto& current_state = plan.steps[i].second.get();
        auto& current_index_dataset = current_state.intermediate_data;
        auto& current_similarity = current_state.intermediate_similarity;
        auto& current_probing_dataset = intermediate_probe_data[i];

        offset_verify_with_similarity(current_index_dataset, index_offset, current_probing_dataset, probe_offset, current_similarity, result_pairs);
        --i;
      }
      verify_with_similarity(dataset.data, similarity, result_pairs);


      timing::ticks end_ticks = timing::cpu_cycles_start();
      auto loss = static_cast<double>(end_ticks - start_ticks);
      bandit.update_weights(plan_id, loss);
      absl::PrintF("Found %u results\n", result_pairs.size());
    }
  }
}

/*void execute_plan(types::Dataset& dataset, similarity::Similarity& similarity, ontology::QueryPlan& plan) {
  std::vector<types::Dataset> intermediate_datasets;
  std::vector<similarity::Similarity> intermediate_similarities;

  for (size_t i = 0; i < plan.reduction_steps.size(); ++i) {
    auto& reduction = plan.reduction_steps[i].get();
    if (i == 0) {
      intermediate_datasets.emplace_back(std::move(reduction.reduce_data(dataset)));
      intermediate_similarities.emplace_back(std::move(reduction.reduce_similarity(similarity)));
    } else {
      intermediate_datasets.emplace_back(std::move(reduction.reduce_data(intermediate_datasets.back())));
      intermediate_similarities.emplace_back(std::move(reduction.reduce_similarity(intermediate_similarities.back())));
    }
  }

  auto& last_dataset = intermediate_datasets.back();
  auto& last_similarity = intermediate_similarities.back();
  auto algorithm = resolve_algorithmid<MaterializeHandler>(plan.algorithm_id, last_similarity);

  auto last_batch = types::dataset_to_batch(last_dataset);
  algorithm->prepare_indexing_batch(last_batch);
  algorithm->index_batch(last_batch);

  std::vector<types::ResultPair> result_pairs;
  MaterializeHandler handler(result_pairs);
  algorithm->join_batch(last_batch, handler);

  // verification is done from lowest to highest reduction level
  // the lowest level at size() - 1 is part of the algorithm step as the algorithm might
  // be able to optimize verification depending on its specifics
  // hence, we can skip verification for size() - 1 and start at size() - 2
  auto i = static_cast<int64_t>(plan.reduction_steps.size()) - 2;
  while (0 <= i) {
    auto& current_dataset = intermediate_datasets[i];
    auto& current_similarity = intermediate_similarities[i];

    verify_with_similarity(current_dataset, current_similarity, result_pairs);
    --i;
  }
  verify_with_similarity(dataset, similarity, result_pairs);

  absl::PrintF("Found %u results\n", result_pairs.size());
}*/

}  // namespace join

#endif  // SRC_PLAN_EXECUTION_HH
