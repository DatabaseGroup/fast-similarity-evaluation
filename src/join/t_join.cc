#include "t_join.hh"

#include "../util/object_ptr.hh"

namespace join {

template <class Handler>
void TJoinLite<Handler>::insert_batch([[maybe_unused]] types::Batch& indexed_data, types::Batch& batch) {
  auto& tree_batch = std::get<types::TreeBatch>(batch);

  label_converter.measureAndAssignFrequencyIdentifiers(tree_batch.data, indexed_sets, token_map_list);

  std::for_each(
    tree_batch.data.begin(), tree_batch.data.end(), [&](auto& tree) { indexed_trees.emplace_back(std::ref(tree)); });

  auto permutation = util::sort_permutation(indexed_sets, [](auto& s1, auto& s2) { return s1.first < s2.first; });
  indexed_sets = util::apply_permutation(indexed_sets, permutation);
  util::apply_permutation_in_place(indexed_trees, permutation);

  index.prepare(token_map_list.size() + 1);

  for (int set_id = 0; set_id < static_cast<int>(indexed_sets.size()); ++set_id) {
    auto& set_data = indexed_set_data.emplace_back();
    set_data.prefix = index.insert(set_id, indexed_sets[set_id].second, ted.threshold);
  }
}

template <class Handler>
bool TJoinLite<Handler>::has_independent_probing_signatures() {
  return true;
}

template <class Handler>
std::any TJoinLite<Handler>::get_probing_signatures(types::Batch& batch) {
  auto& tree_batch = std::get<types::TreeBatch>(batch);

  SetsCollection probing_sets;
  label_converter.convertForeign(tree_batch.data, probing_sets);
  return probing_sets;
}

template <class Handler>
void TJoinLite<Handler>::join_batch([[maybe_unused]] types::Batch& indexed_data,
                                    types::Batch& batch,
                                    Handler handler,
                                    FilterConfig& filter_config,
                                    statistics::JoinStatistics& statistics,
                                    std::shared_ptr<std::any> probing_signatures) {
  auto& tree_batch = std::get<types::TreeBatch>(batch);
  util::object_ptr<std::vector<SetEntry>> probing_sets;
  std::vector<SetEntry> local_sets;

  if (probing_signatures) {
    probing_sets = &std::any_cast<std::vector<SetEntry>&>(*probing_signatures);
  } else {
    local_sets = std::any_cast<std::vector<SetEntry>>(get_probing_signatures(batch));
    probing_sets = &local_sets;
  }

  // this could be done in a nicer way
  switch (filter_config.type) {
  case NOP:
    _join_batch<NopFilter>(tree_batch, true, *probing_sets, handler, filter_config, statistics);
    break;
  case SIMPLE_SELFJOIN:
    _join_batch<SimpleSelfjoinFilter>(tree_batch, true, *probing_sets, handler, filter_config, statistics);
    break;
  case SYMMETRIC_PAIRS:
    _join_batch<SymmetricPairFilter>(tree_batch, true, *probing_sets, handler, filter_config, statistics);
    break;
  case CUTOFF:
  case CUTOFF_SELFJOIN:
    throw std::invalid_argument("TJoinLite does not support cutoff-type filters.");
  }
}

template <class Handler>
template <class Filter>
void TJoinLite<Handler>::_join_batch(types::TreeBatch& trees,
                                     bool probing_signatures_from_cache,
                                     SetsCollection& possibly_cached_probing_sets,
                                     Handler handler,
                                     FilterConfig& filter_config,
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
    index.lookup(r_id, ted.threshold, indexed_sets, probing_sets, indexed_set_data, probing_set_data, join_candidates);
  }

  // handler expects index id to the left
  for (auto& pair : join_candidates) {
    auto& index_tree = indexed_trees[pair.second];
    auto& probe_tree = trees.data[pair.first];

    if (!Filter::scan_skip_cond(index_tree.get(), probe_tree, filter_config) &&
        !Filter::scan_break_cond(index_tree.get(), probe_tree, filter_config)) {
      if (ted.is_in_threshold(index_tree.get(), probe_tree)) {
        handler(index_tree.get().id, probe_tree.id);
      }
    }
  }
  statistics.join_verifications.add(static_cast<int64_t>(join_candidates.size()));
}
}  // namespace join