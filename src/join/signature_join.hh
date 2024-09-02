#ifndef SRC_SIGNATURE_JOIN_HH
#define SRC_SIGNATURE_JOIN_HH

#include <boost/range/irange.hpp>

#include "../indexing/index.hh"
#include "../similarity/signature.hh"
#include "join_algorithm.hh"

namespace join {

using RecordId = int64_t;

template <class DataType>
struct IndexElementGetter {};

template <>
struct IndexElementGetter<types::Set> {
  static types::Set& get(types::Set& set) { return set; }
};

template <>
struct IndexElementGetter<std::reference_wrapper<types::Set>> {
  static types::Set& get(std::reference_wrapper<types::Set>& set) {
    return set.get();
  }
};

template <class DataType>
struct SizeGetter {};

template <>
struct SizeGetter<types::Set> {
  static int64_t get(types::Set& s) {
    return static_cast<int64_t>(s.tokens.size());
  }
};

template <class Filter, class It1, class It2, class Data, class Dataset, class SimilarityType>
void add_small_results(Data& data_ref,
                       It1 small_idx_start,
                       It2 small_idx_end,
                       Dataset& dataset,
                       int64_t minimum_candidate_size,
                       int64_t maximum_candidate_size,
                       SimilarityType& similarity,
                       std::vector<RecordId>& candidates,
                       FilterConfig& filter_config,
                       std::vector<bool>& already_seen) {
  auto& data = IndexElementGetter<Data>::get(data_ref);
  auto always_similar_bound = similarity.always_similar_below_size(data);
  for (; small_idx_start != small_idx_end; ++small_idx_start) {
    auto id = small_idx_start->second;
    auto& candidate_ref = dataset[small_idx_start->second];
    auto& candidate = IndexElementGetter<typename Dataset::value_type>::get(candidate_ref);
    auto candidate_size = SizeGetter<std::decay_t<decltype(candidate)>>::get(candidate);

    if (candidate_size > always_similar_bound || candidate_size > maximum_candidate_size) {
      break;
    }

    if (candidate_size < minimum_candidate_size) {
      continue;
    }

    if (!Filter::scan_skip_cond(candidate, data, filter_config) &&
          !Filter::scan_break_cond(candidate, data, filter_config)) {
      already_seen[id] = true;
      candidates.push_back(id);
    }
  }
}

template <class Handler>
class SignatureJoin : public JoinAlgorithm<Handler> {
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
