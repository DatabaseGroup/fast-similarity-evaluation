#ifndef SRC_PALLOC_JOIN_HH
#define SRC_PALLOC_JOIN_HH

#include "../similarity/similarity.hh"
#include "../util/object_ptr.hh"
#include "result_handler.hh"
#include "signature_join.hh"

namespace join {

template <class Handler, bool ENABLE_DELETION = true>
class PallocJoin : public SignatureJoin<Handler> {
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
  struct SharedState {
    std::vector<SizeGroup> size_groups;
  };
  using RefSet = std::reference_wrapper<types::Set>;
  struct GroupSignatures {
    similarity::PallocSignature::Signatures signatures;
    int64_t group_id{};
  };
  struct CachedSignatures {
    std::vector<GroupSignatures> group_signatures;
  };

public:
  explicit PallocJoin(similarity::Similarity& similarity, SharedState& shared_state)
      : similarity(*std::get<similarity::SetSimilarityPtr>(similarity)),
        shared_state(shared_state),
        size_groups(shared_state.size_groups) {
    // initial size group
    if (size_groups.empty()) {
      int32_t lower_bound = 1;
      int32_t upper_bound = next_size_lb(lower_bound) - 1;
      int32_t partition_count = get_partition_count(lower_bound, upper_bound);
      size_groups.emplace_back(lower_bound, upper_bound, partition_count);
    }
  }

  bool has_independent_probing_signatures() override;
  std::any get_probing_signatures(types::Batch& batch) override;
  void insert_batch(types::Batch& indexed_data, types::Batch& batch) override;
  void join_batch(types::Batch& indexed_data,
                  types::Batch& batch,
                  Handler handler,
                  FilterConfig& filter_config,
                  statistics::JoinStatistics& statistics,
                  std::shared_ptr<std::any> probing_signatures) override;

  bool supports_merge() override { return true; }

  void merge(JoinAlgorithm<Handler>& o) override {
    auto& other = dynamic_cast<PallocJoin&>(o);
    this->index.merge(other.index, this->next_id);
    for (auto [key, value] : other.small_index) {
      this->small_index.emplace(key, this->next_id + value);
    }
    this->next_id += other.next_id;
    this->resize_bitmap(this->next_id);
  }

private:
  template <class Filter>
  void _join_batch(types::Batch& indexed_data,
                   types::Batch& batch,
                   std::vector<CachedSignatures>& signatures,
                   Handler& handler,
                   FilterConfig& filter_config,
                   statistics::JoinStatistics& statistics);

  template <class Filter, class CandidateHandler>
  void _probe_size_group(types::span<types::Set> indexed_sets,
                         types::Set& probing_set,
                         GroupSignatures& group_sigs,
                         SizeGroup& size_group,
                         indexing::ComplexIndex<RecordId, indexing::IndexType::HASH>& size_index,
                         int64_t min_size,
                         int64_t max_size,
                         CandidateHandler& handler,
                         FilterConfig& filter_config,
                         statistics::JoinStatistics& statistics);

  [[nodiscard]] int32_t get_partition_count(int32_t partition_lower_bound, int32_t partition_upper_bound) const {
    int32_t max_probe = similarity.maximum_length_bound(partition_upper_bound);
    int32_t min_probe = similarity.minimum_length_bound(partition_lower_bound);
    auto hd = similarity.max_hd_to(partition_lower_bound, partition_upper_bound, min_probe, max_probe);
    if constexpr (ENABLE_DELETION) {
      hd = hd / 2;
    }
    return static_cast<int32_t>(hd) + 1;
  }
  [[nodiscard]] int32_t next_size_lb(int32_t current_size) const {
    int64_t step = similarity.maximum_length_bound(current_size) - current_size;

    const auto scaled_step = static_cast<int32_t>(static_cast<double>(step) * 1);
    return current_size + scaled_step + 1;
  }

  void append_next_size_group() {
    auto lower_bound = size_groups.back().upper + 1;
    auto upper_bound = next_size_lb(lower_bound) - 1;
    int32_t partition_count = get_partition_count(lower_bound, upper_bound);
    size_groups.emplace_back(lower_bound, upper_bound, partition_count);
  }

  template <class Filter, class Callback>
  void read_filtered(types::span<types::Set> indexed_sets,
                     types::Set& probing_set,
                     std::vector<RecordId>& index_ids,
                     Callback& handler,
                     FilterConfig& filter_config) {
    for (auto id : index_ids) {
      auto& index_set = indexed_sets[id];
      if (Filter::scan_skip_cond(index_set, probing_set, filter_config)) {
        continue;
      }
      if (Filter::scan_break_cond(index_set, probing_set, filter_config)) {
        break;
      }

      handler(id);
    }
  }

private:
  similarity::SetSimilarity& similarity;
  SharedState& shared_state;
  similarity::PallocSignature signature;
  indexing::ComplexIndex<RecordId, indexing::IndexType::ORDERED_RANDOM, indexing::IndexType::HASH> index;
  types::TreeMTable<int32_t, int32_t> small_index;
  std::vector<SizeGroup>& size_groups;
};

template class PallocJoin<MaterializeHandler, true>;
template class PallocJoin<MaterializeHandler, false>;

}  // namespace join

#endif  // SRC_PALLOC_JOIN_HH
