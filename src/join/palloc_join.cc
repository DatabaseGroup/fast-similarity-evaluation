#include "palloc_join.hh"

namespace join {

template <class Handler>
bool PallocJoin<Handler>::has_independent_probing_signatures() {
  return true;
}

template <class Handler>
void PallocJoin<Handler>::insert_batch([[maybe_unused]] types::Batch& batch) {
  auto& sets = std::get<types::SetBatch>(batch);

  indexed_sets.reserve(indexed_sets.size() + sets.data.size());
  indexed_sets.insert(indexed_sets.end(), sets.data.begin(), sets.data.end());
  this->resize_bitmap(indexed_sets.size());

  // initial size group
  if (size_groups.empty()) {
    int32_t lower_bound = 1;
    int32_t upper_bound = next_size_lb(lower_bound) - 1;
    int32_t partition_count = get_partition_count(lower_bound, upper_bound);
    size_groups.emplace_back(lower_bound, upper_bound, partition_count);
  }

  const int64_t max_asbs = similarity.max_asbs();

  for (auto& set : indexed_sets) {
    auto set_size = static_cast<int32_t>(set.get().tokens.size());
    while (size_groups.back().upper < set_size) {
      append_next_size_group();
    }
    auto it = std::lower_bound(
      size_groups.begin(), size_groups.end(), set_size, [](auto& group, auto size) { return group.upper < size; });

    size_t group_idx = std::distance(size_groups.begin(), it);
    auto& group = size_groups[group_idx];
    auto signatures = signature.indexing_signatures(set, group.partition_count);

    for (auto sig : signatures.normal_signatures) {
      index.insert(this->next_id, group_idx, sig);
    }
    for (auto del_sig : signatures.deletion_signatures) {
      index.insert(this->next_id, group_idx, del_sig);
    }

    if (static_cast<int64_t>(set_size) <= max_asbs) {
      small_index.emplace(static_cast<int32_t>(this->next_id), static_cast<int32_t>(set_size));
    }

    ++this->next_id;
  }
}

template <class Handler>
std::any PallocJoin<Handler>::get_probing_signatures(types::Batch& batch) {
  auto& sets = std::get<types::SetBatch>(batch);

  std::vector<CachedSignatures> signatures(sets.data.size());

  for (size_t i = 0; i < sets.data.size(); ++i) {
    auto& set = sets.data[i];
    auto& sig_entry = signatures[i];

    auto set_size = static_cast<int32_t>(set.tokens.size());
    auto min_set_size = similarity.minimum_length_bound(set_size);
    auto max_set_size = similarity.maximum_length_bound(set_size);

    while (size_groups.back().lower < max_set_size) {
      append_next_size_group();
    }

    auto it = std::lower_bound(
      size_groups.begin(), size_groups.end(), min_set_size, [](auto& group, auto size) { return group.upper < size; });
    size_t group_idx = std::distance(size_groups.begin(), it);

    for (size_t grp = group_idx; grp < size_groups.size() && size_groups[grp].lower <= max_set_size; ++grp) {
      auto& e = sig_entry.group_signatures.emplace_back();
      e.group_id = grp;
      e.signatures = signature.indexing_signatures(set, size_groups[grp].partition_count);
    }
  }

  return signatures;
}

template <class Handler>
void PallocJoin<Handler>::join_batch(types::Batch& batch,
                                     Handler handler,
                                     FilterConfig& filter_config,
                                     statistics::JoinStatistics& statistics,
                                     std::shared_ptr<std::any> probing_signatures) {
  util::object_ptr<std::vector<CachedSignatures>> signatures;
  std::vector<CachedSignatures> local_signatures;
  if (probing_signatures) {
    signatures = &std::any_cast<std::vector<CachedSignatures>&>(*probing_signatures);
  } else {
    local_signatures = std::any_cast<std::vector<CachedSignatures>>(get_probing_signatures(batch));
    signatures = &local_signatures;
  }

  // this could be done in a nicer way
  switch (filter_config.type) {
  case NOP:
    _join_batch<NopFilter>(batch, *signatures, handler, filter_config, statistics);
    break;
  case SIMPLE_SELFJOIN:
    _join_batch<SimpleSelfjoinFilter>(batch, *signatures, handler, filter_config, statistics);
    break;
  case SYMMETRIC_PAIRS:
    _join_batch<SymmetricPairFilter>(batch, *signatures, handler, filter_config, statistics);
    break;
  case CUTOFF:
    // todo
    break;
  case CUTOFF_SELFJOIN:
    // todo
    break;
  }
}

template <class Handler>
template <class Filter>
void PallocJoin<Handler>::_join_batch(types::Batch& batch,
                                      std::vector<CachedSignatures>& signatures,
                                      Handler& handler,
                                      FilterConfig& filter_config,
                                      statistics::JoinStatistics& statistics) {
  auto sets = std::get<types::SetBatch>(batch);

  std::vector<bool>& already_seen = this->indexed_bitmap;
  std::vector<RecordId> candidates;

  for (size_t i = 0; i < sets.data.size(); ++i) {
    auto& probing_set = sets.data[i];
    auto& sig = signatures[i];
    auto set_size = static_cast<int64_t>(probing_set.tokens.size());
    auto minimum_size = similarity.minimum_length_bound(set_size);
    auto maximum_size = similarity.maximum_length_bound(set_size);

    // first find sets that might be similar due to size alone
    add_small_results(probing_set,
                      small_index.begin(),
                      small_index.end(),
                      indexed_sets,
                      minimum_size,
                      maximum_size,
                      similarity,
                      candidates,
                      already_seen);

    auto candidate_handler = [&](RecordId set_id) {
      // todo this could be optimized (actually perform the break instead of skipping); lists are maybe short enough
      if (!Filter::scan_skip_cond(indexed_sets[set_id].get(), probing_set, filter_config) ||
          !Filter::scan_break_cond(indexed_sets[set_id].get(), probing_set, filter_config)) {
        if (!already_seen[set_id]) {
          auto index_size = static_cast<int64_t>(indexed_sets[set_id].get().tokens.size());
          if (minimum_size <= index_size && index_size <= maximum_size) {
            already_seen[set_id] = true;
            candidates.push_back(set_id);
          }
        }
      }
    };

    auto index_iter = std::lower_bound(
      index.map.begin(), index.map.end(), sig.group_signatures.front().group_id, [](auto& entry, auto value) {
        return entry.first < value;
      });

    auto last_group = sig.group_signatures.back().group_id;
    auto sig_iter = sig.group_signatures.begin();

    // probe lower signatures
    while (index_iter != index.map.end() && sig_iter != sig.group_signatures.end()) {
      auto& size_index = *index_iter;
      auto group_id = size_index.first;
      if (group_id > last_group) {
        break;
      }
      while (sig_iter->group_id != group_id) {
        ++sig_iter;
      }

      _probe_size_group<Filter::literally_selfjoin()>(
        probing_set, *sig_iter, size_groups[sig_iter->group_id], size_index.second, candidate_handler, statistics);
      ++index_iter;
      ++sig_iter;
    }

    for (auto candidate_id : candidates) {
      // set from indexed data (indexed_sets set in index_batch)
      auto& candidate_set = indexed_sets[candidate_id];

      if constexpr (Filter::literally_selfjoin()) {
        if (probing_set.id <= candidate_set.get().id) {
          already_seen[candidate_id] = false;
          continue;
        }
      }

      if (similarity.is_in_threshold(candidate_set.get(), probing_set)) {
        handler(candidate_set.get().id, probing_set.id);
      }

      already_seen[candidate_id] = false;
    }
    statistics.join_verifications.add(static_cast<int64_t>(candidates.size()));
    candidates.clear();
  }
}

template <class Handler>
template <bool IS_SELF_JOIN, class CandidateHandler>
void PallocJoin<Handler>::_probe_size_group(types::Set& probing_set,
                                            GroupSignatures& group_sigs,
                                            SizeGroup& size_group,
                                            indexing::ComplexIndex<RecordId, indexing::IndexType::HASH>& size_index,
                                            CandidateHandler& handler,
                                            [[maybe_unused]] statistics::JoinStatistics& statistics) {
  std::vector<PartitionCostEntry> costs;
  costs.reserve(size_group.partition_count);
  std::vector<util::object_ptr<std::vector<RecordId>>> normal_ils(size_group.partition_count, nullptr);
  std::vector<util::object_ptr<std::vector<RecordId>>> deletion_ils(group_sigs.signatures.deletion_signatures.size(),
                                                                    nullptr);

  auto& nor_sig = group_sigs.signatures.normal_signatures;
  auto& del_sig = group_sigs.signatures.deletion_signatures;
  auto& del_offset = group_sigs.signatures.deletion_partition_offsets;

  for (int32_t partition = 0; partition < size_group.partition_count; ++partition) {
    auto it = size_index.map.find(nor_sig[partition]);

    int64_t cost = 0;
    if (it != size_index.map.end()) {
      auto& list = it->second;
      cost = static_cast<int64_t>(list.size());
      normal_ils[partition] = util::object_ptr(&list);
    }
    costs.emplace_back(partition, cost, true);
  }

  std::make_heap(costs.begin(), costs.end(), std::greater{});

  auto probing_set_size = static_cast<int32_t>(probing_set.tokens.size());
  const int32_t hamming_distance = similarity.max_hd_to(probing_set_size, size_group.lower, size_group.upper) + 1;
  int32_t remaining = hamming_distance;

  while (0 < remaining) {
    std::pop_heap(costs.begin(), costs.end(), std::greater{});
    auto& entry = costs.back();
    auto partition = entry.partition_id;

    if (entry.is_normal) {
      // 1. read normal il
      if (normal_ils[partition]) {
        for (auto id : *normal_ils[partition]) {
          handler(id);
        }
      }

      int64_t cost = 0;
      // 2. get cost of probing normal signature against deletion index
      {
        auto sig = signature.select_other_index(nor_sig[partition]);
        auto it = size_index.map.find(sig);
        if (it != size_index.map.end()) {
          auto& list = it->second;
          cost += static_cast<int64_t>(list.size());
          normal_ils[partition] = util::object_ptr(&list);
        }
      }

      // 3. get cost of probing deletion signatures against normal index
      for (size_t i = del_offset[partition].begin_offset; i < del_offset[partition].end_offset; ++i) {
        auto sig = signature.select_other_index(del_sig[i]);

        auto it = size_index.map.find(sig);
        if (it != size_index.map.end()) {
          auto& list = it->second;
          cost += static_cast<int64_t>(list.size());
          deletion_ils[i] = util::object_ptr(&list);
        }
      }

      // 4. update heap
      entry.is_normal = false;
      entry.cost = cost;

      std::push_heap(costs.begin(), costs.end(), std::greater{});
    } else {
      if (normal_ils[partition]) {
        for (auto id : *normal_ils[partition]) {
          handler(id);
        }
      }

      for (size_t i = del_offset[partition].begin_offset; i < del_offset[partition].end_offset; ++i) {
        if (deletion_ils[i]) {
          for (auto id : *deletion_ils[i]) {
            handler(id);
          }
        }
      }

      // remove entry from heap
      costs.pop_back();
    }

    --remaining;
  }
}

template <class Handler>
int32_t PallocJoin<Handler>::get_partition_count(int32_t partition_lower_bound, int32_t partition_upper_bound) const {
  return (similarity.max_hd_to(
            partition_upper_bound, partition_lower_bound, similarity.maximum_length_bound(partition_upper_bound)) /
          2) +
         1;
}

}  // namespace join