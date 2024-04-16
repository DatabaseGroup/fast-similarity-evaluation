#ifndef SRC_PALLOC_JOIN_HH
#define SRC_PALLOC_JOIN_HH

#include <experimental/memory>

#include "../similarity/similarity.hh"
#include "signature_join.hh"

namespace join {

template <class Handler>
class PallocJoin : public SignatureJoin<Handler> {
public:
  using RefSet = std::reference_wrapper<types::Set>;
  struct GroupSignatures {
    similarity::PallocSignature::Signatures signatures;
    int64_t group_id{};
  };
  struct CachedSignatures {
    GroupSignatures lower;
    GroupSignatures mid;  // == signatures of own group
    GroupSignatures upper;

    // only has local scope
    size_t probing_set_id{};
  };

private:
  struct SizeGroup {
    int32_t lower;
    int32_t upper;
    int32_t partition_count;

    SizeGroup(int32_t lower, int32_t upper, int32_t partition_count)
        : lower(lower), upper(upper), partition_count(partition_count) {}
  };

  struct PartitionCostEntry {
    int32_t partition_id;
    bool is_normal;
    int64_t cost;

    PartitionCostEntry(int32_t partitionId, int32_t cost, bool isNormal)
        : partition_id(partitionId), is_normal(isNormal), cost(cost) {}

    bool operator<(const PartitionCostEntry& rhs) const { return cost < rhs.cost; }
    bool operator>(const PartitionCostEntry& rhs) const { return rhs < *this; }
  };

public:
  explicit PallocJoin(similarity::Similarity& similarity)
      : similarity(*std::get<similarity::SetSimilarityPtr>(similarity)) {}

  bool has_independent_probing_signatures() override {
    // todo update later
    return false;
  }

  void prepare_indexing_batch(types::Batch& batch) override {
    auto& sets = std::get<types::SetBatch>(batch);

    indexed_sets.reserve(sets.data.size());
    indexed_sets.insert(indexed_sets.begin(), sets.data.begin(), sets.data.end());

    std::sort(indexed_sets.begin(), indexed_sets.end(), [](auto& s1, auto& s2) { return s1.get() < s2.get(); });

    int32_t const min_size = 1;
    auto const max_size = static_cast<int32_t>(indexed_sets.back().get().tokens.size());

    int32_t current_size = min_size;
    while (current_size <= max_size) {
      int32_t upper_bound = similarity.maximum_length_bound(current_size);
      int32_t partition_count = similarity.equivalent_hd(current_size, upper_bound) + 1;
      size_groups.emplace_back(current_size, upper_bound, partition_count);
      current_size = upper_bound + 1;
    }
  }

  void index_batch(types::Batch& batch) override {
    size_t group_idx = 0;

    size_t set_id = 0;
    for (auto& set : indexed_sets) {
      auto set_size = static_cast<int32_t>(set.get().tokens.size());
      while (size_groups[group_idx].lower > set_size) {
        ++group_idx;
      }

      auto& group = size_groups[group_idx];
      auto signatures = signature.indexing_signatures(set, group.partition_count);

      for (auto sig : signatures.normal_signatures) {
        index.insert(set_id, group_idx, sig);
      }
      for (auto del_sig : signatures.deletion_signatures) {
        index.insert(set_id, group_idx, del_sig);
      }

      ++set_id;
    }
  }

  std::any prepare_probing_batch(types::Batch& batch) override {
    auto& sets = std::get<types::SetBatch>(batch);

    std::vector<std::pair<std::reference_wrapper<types::Set>, size_t>> probed_sets;
    for (size_t i = 0; i < sets.data.size(); ++i) {
      probed_sets.emplace_back(sets.data[i], i);
    }
    std::sort(probed_sets.begin(), probed_sets.end(), [](const auto& s1, const auto& s2) {
      return s1.first.get() < s2.first.get();
    });

    std::vector<CachedSignatures> signatures(sets.data.size());

    // this has to be independent of the indexed data
    // group_idx = index of middle--upper group (hence starts at 1)
    size_t group_idx = 2;
    // bound of kind [..., ...) (upper is exclusive)
    int32_t lower_bound = 1;
    int32_t middle_low_bound = similarity.maximum_length_bound(lower_bound) + 1;
    int32_t middle_upper_bound = similarity.maximum_length_bound(middle_low_bound) + 1;
    int32_t upper_bound = similarity.maximum_length_bound(middle_upper_bound) + 1;
    int32_t lower_partition_count = similarity.equivalent_hd(lower_bound, middle_low_bound - 1);
    int32_t mid_partition_count = similarity.equivalent_hd(middle_low_bound, middle_upper_bound - 1);
    ;
    int32_t upper_partition_count = similarity.equivalent_hd(middle_upper_bound, upper_bound - 1);

    for (auto& entry : probed_sets) {
      auto& set = entry.first.get();
      auto& sig_entry = signatures[entry.second];
      sig_entry.probing_set_id = entry.second;
      auto set_size = static_cast<int32_t>(set.tokens.size());
      while (upper_bound < set_size) {
        ++group_idx;
        lower_bound = middle_low_bound;
        middle_low_bound = middle_upper_bound;
        middle_upper_bound = upper_bound;
        upper_bound = similarity.maximum_length_bound(upper_bound) + 1;
        lower_partition_count = mid_partition_count;
        mid_partition_count = upper_partition_count;
        upper_partition_count = similarity.equivalent_hd(middle_upper_bound, upper_bound - 1);
      }

      sig_entry.lower.group_id = group_idx - 2;
      sig_entry.lower.signatures = signature.indexing_signatures(set, lower_partition_count);
      sig_entry.mid.group_id = group_idx - 1;
      sig_entry.mid.signatures = signature.indexing_signatures(set, mid_partition_count);
      sig_entry.upper.group_id = group_idx;
      sig_entry.upper.signatures = signature.indexing_signatures(set, upper_partition_count);
    }

    return signatures;
  }

  void selfjoin_batch(types::Batch& batch,
                      Handler handler,
                      statistics::JoinStatistics& statistics,
                      std::shared_ptr<std::any> probing_signatures) override {
    if (probing_signatures->has_value()) {
      auto& signatures = std::any_cast<std::vector<CachedSignatures>&>(*probing_signatures);
      _join_batch<true>(batch, signatures, handler, statistics);
    } else {
      auto signatures = std::any_cast<std::vector<CachedSignatures>>(prepare_probing_batch(batch));
      _join_batch<true>(batch, signatures, handler, statistics);
    }
  }

  void join_batch(types::Batch& batch,
                  Handler handler,
                  statistics::JoinStatistics& statistics,
                  std::shared_ptr<std::any> probing_signatures) override {
    if (probing_signatures->has_value()) {
      auto& signatures = std::any_cast<std::vector<CachedSignatures>&>(*probing_signatures);
      _join_batch<false>(batch, signatures, handler, statistics);
    } else {
      auto signatures = std::any_cast<std::vector<CachedSignatures>>(prepare_probing_batch(batch));
      _join_batch<false>(batch, signatures, handler, statistics);
    }
  }

  template <bool IS_SELF_JOIN>
  void _join_batch(types::Batch& batch,
                   std::vector<CachedSignatures>& signatures,
                   Handler& handler,
                   statistics::JoinStatistics& statistics) {
    auto sets = std::get<types::SetBatch>(batch);

    std::vector<bool> already_seen(indexed_sets.size());
    std::vector<SetId> candidates;

    for (CachedSignatures& sig : signatures) {
      auto& probing_set = sets.data[sig.probing_set_id];
      auto set_size = static_cast<int64_t>(probing_set.tokens.size());

      auto candidate_handler = [&](SetId set_id) {
        if (!already_seen[set_id]) {
          already_seen[set_id] = true;
          candidates.push_back(set_id);
        }
      };

      auto index_iter = std::lower_bound(
        index.map.begin(),
        index.map.end(),
        sig.lower.group_id,
        [](auto& entry, auto value) { return entry.first > value; });

      auto last_group = IS_SELF_JOIN ? sig.mid.group_id : sig.upper.group_id;
      std::vector<std::reference_wrapper<GroupSignatures>> sig_vector{
        std::ref(sig.lower), std::ref(sig.mid), std::ref(sig.upper)};
      auto sig_iter = sig_vector.begin();

      // probe lower signatures
      while (index_iter != index.map.end() && sig_iter != sig_vector.end()) {
        auto& size_index = *index_iter;
        auto group_id = size_index.first;
        if (group_id > last_group) {
          break;
        }

        _probe_size_group<IS_SELF_JOIN>(probing_set,
                                        (*sig_iter).get(),
                                        size_groups[(*sig_iter).get().group_id],
                                        size_index.second,
                                        candidate_handler,
                                        statistics);
        ++index_iter;
        ++sig_iter;
      }

      for (auto candidate_id : candidates) {
        // set from indexed data (indexed_sets set in index_batch)
        auto& candidate_set = indexed_sets[candidate_id];

        if constexpr (IS_SELF_JOIN) {
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

  template <bool IS_SELF_JOIN, class CandidateHandler>
  void _probe_size_group(types::Set& probing_set,
                         GroupSignatures& group_sigs,
                         SizeGroup& size_group,
                         indexing::ComplexIndex<SetId, indexing::IndexType::HASH>& size_index,
                         CandidateHandler& handler,
                         statistics::JoinStatistics& statistics) {
    std::vector<PartitionCostEntry> costs;
    costs.reserve(size_group.partition_count);
    std::vector<std::experimental::observer_ptr<std::vector<SetId>>> normal_ils(size_group.partition_count);
    std::vector<std::experimental::observer_ptr<std::vector<SetId>>> deletion_ils(
      group_sigs.signatures.deletion_signatures.size());

    auto& nor_sig = group_sigs.signatures.normal_signatures;
    auto& del_sig = group_sigs.signatures.deletion_signatures;
    auto& del_offset = group_sigs.signatures.deletion_partition_offsets;

    for (int32_t partition = 0; partition < size_group.partition_count; ++partition) {
      auto it = size_index.map.find(nor_sig[partition]);

      int64_t cost = 0;
      if (it != size_index.map.end()) {
        auto& list = it->second;
        cost = static_cast<int64_t>(list.size());
        normal_ils[partition] = std::experimental::make_observer(&list);
      }
      costs.emplace_back(partition, cost, true);
    }

    std::make_heap(costs.begin(), costs.end(), std::greater{});

    int32_t hamming_distance = similarity.equivalent_hd(size_group.upper, probing_set.tokens.size()) + 1;

    while (0 < hamming_distance) {
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

        // 2. get cost of deletion ils, update heap
        int64_t cost = 0;
        for (size_t i = del_offset[partition].begin_offset; i < del_offset[partition].end_offset; ++i) {
          auto sig = del_sig[i];

          auto it = size_index.map.find(sig);
          if (it != size_index.map.end()) {
            auto& list = it->second;
            cost += static_cast<int64_t>(list.size());
            deletion_ils[i] = std::experimental::make_observer(&list);
          }
        }

        entry.is_normal = false;
        entry.cost = cost;

        std::push_heap(costs.begin(), costs.end(), std::greater{});
      } else {
        for (size_t i = del_offset[partition].begin_offset; i < del_offset[partition].end_offset; ++i) {
          if (deletion_ils[i]) {
            for (auto id : *deletion_ils[i]) {
              handler(id);
            }
          }
        }
      }

      --hamming_distance;
    }
  }

private:
  similarity::SetSimilarity& similarity;
  std::vector<RefSet> indexed_sets;
  similarity::PallocSignature signature;
  indexing::ComplexIndex<SetId, indexing::IndexType::ORDERED, indexing::IndexType::HASH> index;
  std::vector<SizeGroup> size_groups;
};

}  // namespace join

#endif  // SRC_PALLOC_JOIN_HH
