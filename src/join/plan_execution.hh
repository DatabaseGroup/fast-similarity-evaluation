#ifndef SRC_PLAN_EXECUTION_HH
#define SRC_PLAN_EXECUTION_HH

#include <algorithm>

#include "../ontology/planner.hh"
#include "../similarity/similarity.hh"
#include "../types/types.hh"
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
  }
  return std::make_unique<PrefixSignatureJoin<Handler>>(similarity);
}

class VerifyUntilFailureHandler {
public:
  VerifyUntilFailureHandler(types::Dataset& top_dataset,
                            similarity::Similarity& top_similarity,
                            std::vector<types::Dataset>& datasets,
                            std::vector<similarity::Similarity>& similarities,
                            std::vector<types::ResultPair>& results)
      : datasets(datasets), similarities(similarities), results(results) {}

public:
  void operator()(types::Data::Id id1, types::Data::Id id2) {
    // todo
  }

private:
  std::vector<types::Dataset>& datasets;  // in reverse order
  std::vector<similarity::Similarity>& similarities;  // in reverse order
  std::vector<types::ResultPair>& results;
};

class MaterializeHandler {
public:
  explicit MaterializeHandler(std::vector<types::ResultPair>& results) : results(results) {}

  void operator()(types::Data::Id id1, types::Data::Id id2) { results.emplace_back(id1, id2); }

public:
  std::vector<types::ResultPair>& results;
};

template <class DataType, class SimilarityPtr>
void _verify(DataType& dataset, SimilarityPtr& similarity, std::vector<types::ResultPair>& pairs) {
  /*auto it = pairs.data();
  auto it_end = pairs.data() + pairs.size();

  while (it != it_end) {
    auto& pair = *it;
    auto l_id = pair.first;
    auto r_id = pair.second;

    auto& l = dataset[l_id];
    auto& r = dataset[r_id];

    auto is_similar = similarity->is_in_threshold(l, r);

    if (is_similar) {
      ++it;
    } else {
      --it_end;
      std::swap(it, it_end);
    }
  }*/
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

// magic from https://en.cppreference.com/w/cpp/utility/variant/visit
// helper type for the visitor #4
template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
// explicit deduction guide (not needed as of C++20)
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

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

  std::visit(overloaded{set_verify, string_verify, tree_verify}, dataset);
}

void execute_plan(types::Dataset& dataset, similarity::Similarity& similarity, ontology::QueryPlan& plan) {
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

  algorithm->prepare_dataset(last_dataset);
  algorithm->index_dataset(last_dataset);

  std::vector<types::ResultPair> result_pairs;
  MaterializeHandler handler(result_pairs);
  algorithm->join_dataset(last_dataset, handler);

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
}

}  // namespace join

#endif  // SRC_PLAN_EXECUTION_HH
