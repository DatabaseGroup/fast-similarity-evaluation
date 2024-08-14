#include "t_join.hh"

namespace join {

template <class Handler, class Filter>
void TJoinLite<Handler, Filter>::insert_batch([[maybe_unused]] types::Batch& batch) {
  auto& tree_batch = std::get<types::TreeBatch>(batch);

  label_converter.measureAndAssignFrequencyIdentifiers(tree_batch.data, indexed_sets, token_map_list);

  std::for_each(tree_batch.data.begin(), tree_batch.data.end(), [&](auto& tree) { indexed_trees.emplace_back(std::ref(tree)); });

  auto permutation = util::sort_permutation(indexed_sets, [](auto& s1, auto& s2) { return s1.first < s2.first; });
  indexed_sets = util::apply_permutation(indexed_sets, permutation);
  util::apply_permutation_in_place(indexed_trees, permutation);

  index.prepare(token_map_list.size() + 1);

  for (int set_id = 0; set_id < static_cast<int>(indexed_sets.size()); ++set_id) {
    auto& set_data = indexed_set_data.emplace_back();
    set_data.prefix = index.insert(set_id, indexed_sets[set_id].second, ted.threshold);
  }
}

template <class Handler, class Filter>
bool TJoinLite<Handler, Filter>::has_independent_probing_signatures() { return true; }

template <class Handler, class Filter>
std::any TJoinLite<Handler, Filter>::get_probing_signatures(types::Batch& batch) {
  auto& tree_batch = std::get<types::TreeBatch>(batch);

  SetsCollection probing_sets;
  label_converter.convertForeign(tree_batch.data, probing_sets);
  return probing_sets;
}

template <class Handler, class Filter>
void TJoinLite<Handler, Filter>::selfjoin_batch(types::Batch& batch,
                    Handler handler,
                    statistics::JoinStatistics& statistics,
                    std::shared_ptr<std::any> probing_signatures) {
  auto& tree_batch = std::get<types::TreeBatch>(batch);

  if (probing_signatures) {
    auto& probing_sets = std::any_cast<std::vector<SetEntry>&>(*probing_signatures);
    _join_batch<true>(tree_batch, true, probing_sets, handler, statistics);
  } else {
    auto probing_sets = std::any_cast<std::vector<SetEntry>>(get_probing_signatures(batch));
    _join_batch<true>(tree_batch, false, probing_sets, handler, statistics);
  }
}

template <class Handler, class Filter>
void TJoinLite<Handler, Filter>::join_batch(types::Batch& batch,
                Handler handler,
                statistics::JoinStatistics& statistics,
                std::shared_ptr<std::any> probing_signatures) {
  auto& tree_batch = std::get<types::TreeBatch>(batch);

  if (probing_signatures) {
    auto& probing_sets = std::any_cast<std::vector<SetEntry>&>(*probing_signatures);
    _join_batch<false>(tree_batch, true, probing_sets, handler, statistics);
  } else {
    auto probing_sets = std::any_cast<std::vector<SetEntry>>(get_probing_signatures(batch));
    _join_batch<false>(tree_batch, false, probing_sets, handler, statistics);
  }
}

template <class Handler, class Filter>
template <bool IS_SELF_JOIN>
void TJoinLite<Handler, Filter>::_join_batch(types::TreeBatch& trees,
                 bool probing_signatures_from_cache,
                 SetsCollection& possibly_cached_probing_sets,
                 Handler handler,
                 statistics::JoinStatistics& statistics) {
  SetsCollection probing_sets;
  if (probing_signatures_from_cache) {
    // we cannot modify them here, take copy
    probing_sets = possibly_cached_probing_sets;
    label_converter.assignForeignFrequencyIdentifiers(probing_sets, token_map_list);
  } else {
    // we can move them
    label_converter.assignForeignFrequencyIdentifiers(possibly_cached_probing_sets, token_map_list);
    probing_sets = std::move(possibly_cached_probing_sets);
  }

  std::vector<SetData> probing_set_data(probing_sets.size());
  // contains pairs (r_id, s_id) where r_id is a probing set id and s_id is an indexed set id
  std::vector<std::pair<int, int>> join_candidates;

  for (int r_id = 0; r_id < static_cast<int>(trees.data.size()); ++r_id) {
    index.lookup(
      r_id, ted.threshold, indexed_sets, probing_sets, indexed_set_data, probing_set_data, join_candidates);
  }

  // handler expects index id to the left
  for (auto& pair : join_candidates) {
    auto& index_tree = indexed_trees[pair.second];
    auto& probe_tree = trees.data[pair.first];

    if (Filter::tree_pred(index_tree.get(), probe_tree)) {
      // self-joins skip symmetric ids
      if constexpr (IS_SELF_JOIN) {
        if (index_tree.get().id >= probe_tree.id) {
          continue;
        }
      }
      if (ted.is_in_threshold(index_tree.get(), probe_tree)) {
        handler(index_tree.get().id, probe_tree.id);
      }
    }
  }
  statistics.join_verifications.add(static_cast<int64_t>(join_candidates.size()));
}
}