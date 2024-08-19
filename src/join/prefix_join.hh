#ifndef SRC_PREFIX_JOIN_HH
#define SRC_PREFIX_JOIN_HH

#include "../similarity/similarity.hh"
#include "result_handler.hh"
#include "signature_join.hh"

namespace join {

// Used to support add_small_results for PrefixSignature
template <>
struct SizeGetter<types::Set> {
  static int64_t get_size(types::Set& set) { return static_cast<int64_t>(set.tokens.size()); }
};

template <class Handler>
class PrefixSignatureJoin : public SignatureJoin<Handler> {
public:
  struct SharedState {
    similarity::SetQuasiSuffix sqs;
    int64_t sqs_version = 0;
    int64_t totally_indexed_sets = 0;
    int64_t next_reindexing = 0;
  };

public:
  explicit PrefixSignatureJoin(similarity::Similarity& similarity, SharedState& shared_state)
      : similarity(*std::get<similarity::SetSimilarityPtr>(similarity)),
        shared_state(shared_state),
        prefix_signature(*std::get<similarity::SetSimilarityPtr>(similarity), shared_state.sqs) {}

  void insert_batch(types::Batch& batch) override;
  void join_batch(types::Batch& batch,
                  Handler handler,
                  FilterConfig& filter_config,
                  statistics::JoinStatistics& statistics,
                  std::shared_ptr<std::any> probing_signatures) override;

  template <class Filter>
  void _join_batch(types::Batch& batch,
                   Handler handler,
                   FilterConfig& filter_config,
                   statistics::JoinStatistics& statistics);

private:
  void insert_into_index(types::span<types::Set> sets);

private:
  similarity::SetSimilarity& similarity;
  SharedState& shared_state;
  int64_t local_sqs_version = 0;
  similarity::SetPrefixSignature prefix_signature;
  indexing::ComplexIndex<RecordId, indexing::IndexType::HASH, indexing::IndexType::ORDERED_RANDOM> index{};
  types::TreeMTable<int32_t, int32_t> small_index;
  std::vector<std::reference_wrapper<types::Set>> indexed_sets;
  std::vector<types::Set> preprocessed_sets;
};

template class PrefixSignatureJoin<MaterializeHandler>;

}  // namespace join

#endif  // SRC_PREFIX_JOIN_HH
