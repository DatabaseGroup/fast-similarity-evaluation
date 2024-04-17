#ifndef SRC_PALLOC_JOIN_HH
#define SRC_PALLOC_JOIN_HH

#include <experimental/memory>

#include "../similarity/similarity.hh"
#include "result_handler.hh"
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
    std::vector<GroupSignatures> group_signatures;

    // only has local scope
    int32_t probing_set_id{};
    int32_t own_group_id{};
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

  bool has_independent_probing_signatures() override;
  std::any prepare_probing_batch(types::Batch& batch) override;
  void prepare_indexing_batch(types::Batch& batch) override;
  void index_batch(types::Batch& batch) override;
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
                         indexing::ComplexIndex<SetId, indexing::IndexType::HASH>& size_index,
                         CandidateHandler& handler,
                         statistics::JoinStatistics& statistics);

private:
  similarity::SetSimilarity& similarity;
  std::vector<RefSet> indexed_sets;
  similarity::PallocSignature signature;
  indexing::ComplexIndex<SetId, indexing::IndexType::ORDERED, indexing::IndexType::HASH> index;
  std::vector<SizeGroup> size_groups;
};

template class PallocJoin<MaterializeHandler>;

}  // namespace join

#endif  // SRC_PALLOC_JOIN_HH
