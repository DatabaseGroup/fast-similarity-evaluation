#ifndef SRC_SIGNATURE_JOIN_HH
#define SRC_SIGNATURE_JOIN_HH

#include <boost/range/irange.hpp>

#include "../indexing/index.hh"
#include "../similarity/signature.hh"
#include "join_algorithm.hh"

namespace join {

using RecordId = int64_t;

template <class DataType>
struct SizeGetter {};

template <class It1, class It2, class Datatype, class SimilarityType>
void add_small_results(const typename Datatype::value_type& data,
                       It1 small_idx_start,
                       It2 small_idx_end,
                       Datatype& dataset,
                       int64_t minimum_candidate_size,
                       int64_t maximum_candidate_size,
                       SimilarityType& similarity,
                       std::vector<RecordId>& candidates,
                       std::vector<bool>& already_seen) {
  auto always_similar_bound = similarity.always_similar_below_size(data);
  for (; small_idx_start != small_idx_end; ++small_idx_start) {
    auto id = small_idx_start->second;
    auto& candidate = dataset[small_idx_start->second];
    auto candidate_size = SizeGetter<typename Datatype::value_type>::get_size(candidate);

    if (candidate_size > always_similar_bound || candidate_size > maximum_candidate_size) {
      break;
    }

    if (candidate_size < minimum_candidate_size) {
      continue;
    }

    already_seen[id] = true;
    candidates.push_back(id);
  }
}

template <class Handler, class Filter = NopFilter>
class SignatureJoin : public JoinAlgorithm<Handler, Filter> {
public:
  void insert_batch(types::Batch& batch) = 0;

  void selfjoin_batch(types::Batch& batch,
                      Handler handler,
                      statistics::JoinStatistics& statistics,
                      std::shared_ptr<std::any> probing_signatures) = 0;
  void join_batch(types::Batch& batch,
                  Handler handler,
                  statistics::JoinStatistics& statistics,
                  std::shared_ptr<std::any> probing_signatures) = 0;

protected:
  void resize_bitmap(uint64_t new_size) {
    if (indexed_bitmap.size() < new_size) {
      indexed_bitmap.resize(static_cast<uint64_t>(std::exp2(std::ceil(std::log2(new_size)))));
    }
  }

protected:
  RecordId next_id = 0;
  std::vector<bool> indexed_bitmap;
};

}  // namespace join

#endif  // SRC_SIGNATURE_JOIN_HH
