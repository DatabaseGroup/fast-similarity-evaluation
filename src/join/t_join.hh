#ifndef SRC_T_JOIN_HH
#define SRC_T_JOIN_HH

#include <tsim/join/tjoin/candidate_index.h>
#include <tsim/join/tjoin/label_set_converter.h>

#include "../similarity/similarity.hh"
#include "../util/permutation.hh"
#include "result_handler.hh"
#include "join_algorithm.hh"

namespace tsim::label_set_converter {

template <>
struct TreeGetter<types::Tree> {
  static const types::Tree::Node& get(const types::Tree& tree) { return tree.root; }
};

}  // namespace tsim::label_set_converter

namespace join {

// TJoin has its reduction somewhat hidden inside its implementation, so this implements both reducing and joining
template <class Handler>
class TJoinLite : public JoinAlgorithm<Handler> {
private:
  using Label = types::Tree::Label;
  using CandidateIndex = tsim::candidate_index::CandidateIndex;
  using Converter = tsim::label_set_converter::Converter<Label>;
  using TokenFrequencyMap = std::unordered_map<int, int>;
  using SetEntry = std::pair<int, std::vector<tsim::label_set_converter::LabelSetElement>>;
  using SetsCollection = std::vector<SetEntry>;
  using SetData = tsim::candidate_index::SetData;

public:
  explicit TJoinLite(similarity::Similarity& similarity)
      : ted(dynamic_cast<similarity::TreeEditDistance&>(*std::get<similarity::TreeSimilarityPtr>(similarity))) {}

public:
  void prepare_indexing_batch(types::Batch& batch) override;
  bool has_independent_probing_signatures() override;
  std::any prepare_probing_batch(types::Batch& batch) override;
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
  void _join_batch(types::TreeBatch& trees,
                   bool probing_signatures_from_cache,
                   SetsCollection& possibly_cached_probing_sets,
                   Handler handler,
                   statistics::JoinStatistics& statistics);

private:
  similarity::TreeEditDistance& ted;
  CandidateIndex index;
  SetsCollection indexed_sets;
  // parallel to indexed_sets
  std::vector<SetData> indexed_set_data;
  std::vector<std::reference_wrapper<types::Tree>> indexed_trees;
  Converter label_converter;
  TokenFrequencyMap token_map_list;
};

template class TJoinLite<MaterializeHandler>;

}  // namespace join

#endif  // SRC_T_JOIN_HH
