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

template <class Handler, class Filter = NopFilter>
class PrefixSignatureJoin : public SignatureJoin<Handler, Filter> {
public:
  explicit PrefixSignatureJoin(similarity::Similarity& similarity)
      : similarity(*std::get<similarity::SetSimilarityPtr>(similarity)),
        prefix_signature(*std::get<similarity::SetSimilarityPtr>(similarity)) {}

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

  template <bool IS_SELF_JOIN>
  void _join_batch(types::Batch& batch, Handler handler, statistics::JoinStatistics& statistics);

private:
  similarity::SetSimilarity& similarity;
  similarity::SetPrefixSignature prefix_signature;
  indexing::ComplexIndex<SetId, indexing::IndexType::DISCRETE, indexing::IndexType::ORDERED> index{0};
  std::vector<types::Set> indexed_sets;
};

template class PrefixSignatureJoin<MaterializeHandler, NopFilter>;
template class PrefixSignatureJoin<MaterializeHandler, SymmetricPairFilter>;

}

#endif  // SRC_PREFIX_JOIN_HH
