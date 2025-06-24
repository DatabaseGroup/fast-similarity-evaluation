#ifndef SRC_PREFIX_JOIN_HH
#define SRC_PREFIX_JOIN_HH

#include "../similarity/similarity.hh"
#include "../util/object_ptr.hh"
#include "result_handler.hh"
#include "signature_join.hh"

namespace join {

template <class Handler, bool PRESORTED = false>
class PrefixSignatureJoin : public SignatureJoin<Handler> {
public:
  struct SharedState {
    similarity::SetQuasiSuffix sqs;
    int64_t sqs_version = 0;
    int64_t totally_indexed_tokens = 0;
    int64_t next_reindexing = 50000;
  };
  struct CachedSignatures {
    std::vector<types::Set> prepared_sets;
    int64_t sqs_version = -1;
  };

public:
  explicit PrefixSignatureJoin(similarity::Similarity& similarity, SharedState& shared_state)
      : similarity(*std::get<similarity::SetSimilarityPtr>(similarity)),
        shared_state(shared_state),
        prefix_signature(*std::get<similarity::SetSimilarityPtr>(similarity), shared_state.sqs) {}

  bool has_independent_probing_signatures() override { return true; }
  std::any get_probing_signatures(types::Batch& batch) override;
  void insert_batch(types::Batch& indexed_data, types::Batch& batch) override;
  void join_batch(types::Batch& indexed_data,
                  types::Batch& batch,
                  Handler handler,
                  FilterConfig& filter_config,
                  statistics::JoinStatistics& statistics,
                  std::shared_ptr<std::any> probing_signatures) override;

  template <class Filter>
  void _join_batch(CachedSignatures& signatures,
                   Handler handler,
                   FilterConfig& filter_config,
                   statistics::JoinStatistics& statistics);

private:
  void insert_into_index(types::span<types::Set> sets);
  void update_index(types::span<types::Set>& indexed_sets);

private:
  using IndexType = std::conditional_t<
    PRESORTED,
    indexing::ComplexIndex<RecordId, indexing::IndexType::HASH, indexing::IndexType::ORDERED_PRESORTED>,
    indexing::ComplexIndex<RecordId, indexing::IndexType::HASH, indexing::IndexType::ORDERED_RANDOM>>;

private:
  similarity::SetSimilarity& similarity;
  SharedState& shared_state;
  int64_t local_sqs_version = 0;
  similarity::SetPrefixSignature prefix_signature;
  IndexType index{};
  types::TreeMTable<int32_t, int32_t> small_index;
  std::vector<types::Set> preprocessed_sets;
};

template class PrefixSignatureJoin<MaterializeHandler, false>;
template class PrefixSignatureJoin<MaterializeHandler, true>;

}  // namespace join

#endif  // SRC_PREFIX_JOIN_HH
