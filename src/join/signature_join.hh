#ifndef SRC_SIGNATURE_JOIN_HH
#define SRC_SIGNATURE_JOIN_HH

#include <boost/range/irange.hpp>

#include "../indexing/index.hh"
#include "../similarity/signature.hh"
#include "join_algorithm.hh"

namespace join {

using SetId = int64_t;

template <class DataType>
struct SizeGetter {};

template <class DataType, class SimilarityType>
inline void add_small_results(typename DataType::value_type data,
                              DataType& indexed_data,
                              int64_t minimum_candidate_size,
                              int64_t maximum_candidate_size,
                              SimilarityType& similarity,
                              std::vector<SetId>& candidates,
                              std::vector<bool>& already_seen) {
  auto always_similar_bound = similarity.always_similar_below_size(data);
  for (int64_t i = 0; i < static_cast<int64_t>(indexed_data.size()); ++i) {
    auto& candidate = indexed_data[i];
    auto candidate_size = SizeGetter<typename DataType::value_type>::get_size(candidate);

    if (candidate_size > always_similar_bound || candidate_size > maximum_candidate_size) {
      break;
    }

    if (candidate_size < minimum_candidate_size) {
      continue;
    }

    already_seen[i] = true;
    candidates.push_back(i);
  }
}

template <class Handler, class Filter = NopFilter>
class SignatureJoin : public JoinAlgorithm<Handler, Filter> {
public:
  void prepare_indexing_batch(types::Batch& batch) = 0;
  void index_batch(types::Batch& batch) = 0;

  void selfjoin_batch(types::Batch& batch,
                      Handler handler,
                      statistics::JoinStatistics& statistics,
                      std::shared_ptr<std::any> probing_signatures) = 0;
  void join_batch(types::Batch& batch,
                  Handler handler,
                  statistics::JoinStatistics& statistics,
                  std::shared_ptr<std::any> probing_signatures) = 0;
};

}  // namespace join

#endif  // SRC_SIGNATURE_JOIN_HH
