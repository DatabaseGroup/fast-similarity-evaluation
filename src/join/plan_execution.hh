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
#include "../statistics/join_statistics.hh"

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

int64_t get_offset_into_batch(int64_t batch_idx, int64_t batch_size) { return batch_idx * batch_size; }

void interleave_plans(data::Dataset& dataset,
                      similarity::Similarity& similarity,
                      std::vector<ontology::QueryPlan>& plans,
                      int64_t block_size,
                      statistics::JoinStatistics& statistics) {
  int64_t batch_count = static_cast<int64_t>(dataset.statistics->count) / block_size;
  if (dataset.statistics->count % block_size != 0) {
    ++batch_count;
  }
  int64_t all_batch_pairs = (batch_count * (batch_count - 1)) / 2;
  ontology::Exp3LightA bandit(static_cast<int64_t>(plans.size()), all_batch_pairs);

  for (int64_t index_batch_idx = 0; index_batch_idx < batch_count; ++index_batch_idx) {
    auto index_batch = types::get_batch(dataset.data, index_batch_idx, block_size);
    auto index_offset = get_offset_into_batch(index_batch_idx, block_size);
    std::vector<std::unique_ptr<join::JoinAlgorithm<MaterializeHandler>>> algorithms(plans.size());

    for (int64_t probe_batch_idx = index_batch_idx; probe_batch_idx < batch_count; ++probe_batch_idx) {
      auto probe_batch = types::get_batch(dataset.data, probe_batch_idx, block_size);
      auto probe_offset = get_offset_into_batch(probe_batch_idx, block_size);

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

        plan.query_state.algorithm_prepared = index_batch_idx;
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

      if (index_batch_idx == probe_batch_idx) {
        algorithm->selfjoin_batch(last_probe_batch, handler, statistics);
      } else {
        algorithm->join_batch(last_probe_batch, handler, statistics);
      }

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

        statistics.filter_verifications.add(static_cast<int64_t>(result_pairs.size()));
        offset_verify_with_similarity(current_index_dataset, index_offset, current_probing_dataset, probe_offset, current_similarity, result_pairs);
        --i;
      }
      statistics.filter_verifications.add(static_cast<int64_t>(result_pairs.size()));
      verify_with_similarity(dataset.data, similarity, result_pairs);


      timing::ticks end_ticks = timing::cpu_cycles_start();
      auto loss = static_cast<double>(end_ticks - start_ticks);
      bandit.update_weights(plan_id, loss);
      statistics.result_size.add(static_cast<int64_t>(result_pairs.size()));
    }
  }
}

}  // namespace join

#endif  // SRC_PLAN_EXECUTION_HH
