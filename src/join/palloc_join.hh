#ifndef SRC_PALLOC_JOIN_HH
#define SRC_PALLOC_JOIN_HH

#include "../similarity/similarity.hh"
#include "../util/object_ptr.hh"
#include "result_handler.hh"
#include "signature_join.hh"

namespace join {

// Used to support add_small_results in PassJoin
template <>
struct SizeGetter<std::reference_wrapper<types::Set>> {
  static int64_t get_size(std::reference_wrapper<types::Set>& set) {
    return static_cast<int64_t>(set.get().tokens.size());
  }
};

template <class Handler, class Filter = NopFilter>
class PallocJoin : public SignatureJoin<Handler, Filter> {
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
      : similarity(*std::get<similarity::SetSimilarityPtr>(similarity)), shared_state(shared_state), size_groups(shared_state.size_groups) {}

  bool has_independent_probing_signatures() override;
  std::any get_probing_signatures(types::Batch& batch) override;
  void insert_batch(types::Batch& batch) override;
  void selfjoin_batch(types::Batch& batch,
                      Handler handler,
                      statistics::JoinStatistics& statistics,
                      std::shared_ptr<std::any> probing_signatures) override;
  void join_batch(types::Batch& batch,
                  Handler handler,
                  statistics::JoinStatistics& statistics,
                  std::shared_ptr<std::any> probing_signatures) override;

private:
  template <bool IS_SELF_JOIN>
  void _join_batch(types::Batch& batch,
              std::vector<CachedSignatures>& signatures,
              Handler& handler,
              statistics::JoinStatistics& statistics);

  template <bool IS_SELF_JOIN, class CandidateHandler>
  void _probe_size_group(types::Set& probing_set,
                         GroupSignatures& group_sigs,
                         SizeGroup& size_group,
                         indexing::ComplexIndex<RecordId, indexing::IndexType::HASH>& size_index,
                         CandidateHandler& handler,
                         statistics::JoinStatistics& statistics);

  int32_t get_partition_count(int32_t partition_lower_bound, int32_t partition_upper_bound) const;
  [[nodiscard]] int32_t next_size_lb(int32_t current_size) const {
    auto step = similarity.maximum_length_bound(current_size) - current_size;
    auto scaled_step = static_cast<int32_t>(static_cast<double>(step) * 1);
    return current_size + scaled_step + 1;
  }

  void append_next_size_group() {
    auto lower_bound = size_groups.back().upper + 1;
    auto upper_bound = next_size_lb(lower_bound) - 1;
    int32_t partition_count = get_partition_count(lower_bound, upper_bound);
    size_groups.emplace_back(lower_bound, upper_bound, partition_count);
  }

private:
  similarity::SetSimilarity& similarity;
  SharedState& shared_state;
  std::vector<RefSet> indexed_sets;
  similarity::PallocSignature signature;
  indexing::ComplexIndex<RecordId, indexing::IndexType::ORDERED_RANDOM, indexing::IndexType::HASH> index;
  types::TreeMTable<int32_t, int32_t> small_index;
  std::vector<SizeGroup>& size_groups;
};

template class PallocJoin<MaterializeHandler, NopFilter>;
template class PallocJoin<MaterializeHandler, SymmetricPairFilter>;

}  // namespace join

#endif  // SRC_PALLOC_JOIN_HH
