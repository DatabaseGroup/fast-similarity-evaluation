#ifndef SRC_REDUCTION_HH
#define SRC_REDUCTION_HH

#include <ranges>

#include "../similarity/similarity.hh"
#include "../types/types.hh"
#include "../util/hashing.hh"
#include "../util/visit_overload.hh"

namespace ontology {

class Reduction {
public:
  virtual ~Reduction() = default;

protected:
  template <class InDataset, class OutDatasetType>
  static types::Dataset forward_as_batch(InDataset& dataset, Reduction& reduction) {
    return std::visit(
      [&](auto&& data) {
        using DatasetType = std::decay_t<decltype(data)>;
        return types::Dataset(_reduce_like_batch<DatasetType, OutDatasetType>(data, reduction));
      },
      dataset);
  }

private:
  // InDatasetType = types::DataMeta<types::Set>, types::DataMeta<types::String>, or types::DataMeta<types::Tree>
  // OutDatasetType = types::DataMeta<types::Set>, types::DataMeta<types::String>, or types::DataMeta<types::Tree>
  template <class InDatasetType, class OutDatasetType>
  static OutDatasetType _reduce_like_batch(InDatasetType& dataset, Reduction& reduction) {
    // Datatype should be types::Set/String/Tree
    using InDatatype = typename InDatasetType::value_type;
    using OutDatatype = typename OutDatasetType::value_type;

    OutDatasetType reduced_data;
    reduced_data.data.resize(dataset.data.size());

    types::span<InDatatype> data_span{dataset.data};
    types::Batch data_batch(types::DataBatch<InDatatype>(data_span, dataset.meta));
    types::span<OutDatatype> reduced_span{reduced_data.data};
    types::Batch reduced_batch{types::DataBatch<OutDatatype>(reduced_data)};
    reduction.reduce_data(data_batch, reduced_batch);

    return reduced_data;
  }

public:
  // Assert: input data and output data have the same ids
  virtual types::Dataset reduce_data(types::Dataset& dataset) = 0;
  virtual types::Dataset reduce_data(types::Batch& input_batch) = 0;
  virtual void reduce_data(types::Batch& input_batch, types::Batch& output_batch) = 0;
  virtual similarity::Similarity reduce_similarity(similarity::Similarity& similarity) = 0;
  [[nodiscard]] virtual std::string get_label() const = 0;
};

class QGramReduction : public Reduction {
public:
  explicit QGramReduction(int32_t q) : q(q) {}
  void reduce_data(types::Batch& input_batch, types::Batch& output_batch) override {
    assert(std::holds_alternative<types::StringBatch>(input_batch) &&
           std::holds_alternative<types::SetBatch>(output_batch));
    auto& in_strings = std::get<types::StringBatch>(input_batch);
    auto& out_strings = std::get<types::SetBatch>(output_batch);
    assert(in_strings.data.size() == out_strings.data.size());

    auto in_iter = in_strings.data.begin();
    auto in_iter_end = in_strings.data.end();
    auto out_iter = out_strings.data.begin();
    // out_iter_end is reached exactly when in_iter_end is reached as both spans have the same size

    while (in_iter != in_iter_end) {
      auto& string = *in_iter;
      auto& set = *out_iter;

      set.id = string.id;
      generate_qgrams(string, set);

      ++in_iter;
      ++out_iter;
    }
  }

  types::Dataset reduce_data(types::Dataset& dataset) override {
    return Reduction::forward_as_batch<types::Dataset, types::Sets>(dataset, *this);
  }

  types::Dataset reduce_data(types::Batch& input_batch) override {
    return Reduction::forward_as_batch<types::Batch, types::Sets>(input_batch, *this);
  }

  similarity::Similarity reduce_similarity(similarity::Similarity& similarity) override {
    assert(std::holds_alternative<similarity::StringSimilarityPtr>(similarity));

    // assert: Similarity is String Edit Distance
    auto& sed =
      dynamic_cast<similarity::StringEditDistance&>(std::get<similarity::StringSimilarityPtr>(similarity).operator*());

    similarity::Similarity qgc_sim(std::make_unique<similarity::QGramCountSimilarity>(sed.threshold, this->q));
    return qgc_sim;
  }

  std::string get_label() const override { return std::to_string(q) + "gram"; }

private:
  static int64_t mask_highest_bit(const uint64_t n) { return static_cast<int64_t>(n & ~(1uLL << 63)); }

public:
  void generate_qgrams(types::String& string, types::Set& set) const {
    util::RabinFingerprint<types::String::str_t::value_type> rf{q};
    set.tokens.reserve(string.str.size() + q - 1);

    for (int32_t i = 1; i < q; ++i) {
      rf.roll(PADDING);
    }

    for (int32_t i = 0; i < static_cast<int32_t>(string.str.size()); ++i) {
      auto token = mask_highest_bit(rf.roll(string.str[i]));
      set.tokens.push_back(token);
      if (i - rf.get_window() + 1 >= 0) {
        rf.remove(string.str[i - rf.get_window() + 1]);
      } else {
        rf.remove(PADDING);
      }
    }

    for (int32_t i = q - 2; i >= 1; --i) {
      int64_t token = mask_highest_bit(rf.roll(PADDING));
      set.tokens.push_back(token);

      rf.remove(*(string.str.end() - i - 1));
    }
    int64_t token = mask_highest_bit(rf.roll(PADDING));
    set.tokens.push_back(token);
  }

private:
  int32_t q;

  static const types::String::str_t::value_type PADDING = 0;
};

class TraversalStringReduction : public Reduction {
public:
  void reduce_data(types::Batch& input_batch, types::Batch& output_batch) override {
    assert(std::holds_alternative<types::TreeBatch>(input_batch) &&
           std::holds_alternative<types::StringBatch>(output_batch));
    auto& in_trees = std::get<types::TreeBatch>(input_batch);
    auto& out_strings = std::get<types::StringBatch>(output_batch);
    assert(in_trees.data.size() == out_strings.data.size());

    auto in_iter = in_trees.data.begin();
    auto in_iter_end = in_trees.data.end();
    auto out_iter = out_strings.data.begin();
    // out_iter_end is reached exactly when in_iter_end is reached as both spans have the same size

    while (in_iter != in_iter_end) {
      auto& tree = *in_iter;
      auto& string = *out_iter;

      string.id = tree.id;
      generate_preorder(in_trees.meta.label_dict, tree, string);

      ++in_iter;
      ++out_iter;
    }
  }
  similarity::Similarity reduce_similarity(similarity::Similarity& similarity) override {
    assert(std::holds_alternative<similarity::TreeSimilarityPtr>(similarity));

    // assert: Similarity is Tree Edit Distance
    auto& ted =
      dynamic_cast<similarity::TreeEditDistance&>(std::get<similarity::TreeSimilarityPtr>(similarity).operator*());

    similarity::Similarity sed(std::make_unique<similarity::StringEditDistance>(ted.threshold));
    return sed;
  }
  std::string get_label() const override { return "traversal_strings"; }

  types::Dataset reduce_data(types::Dataset& dataset) override {
    return Reduction::forward_as_batch<types::Dataset, types::Strings>(dataset, *this);
  }

  types::Dataset reduce_data(types::Batch& input_batch) override {
    return Reduction::forward_as_batch<types::Batch, types::Strings>(input_batch, *this);
  }

private:
  void generate_preorder(types::Tree::LabelDictionary& ld, types::Tree& tree, types::String& string) {
    std::vector<std::reference_wrapper<const types::Tree::Node>> queue;
    queue.emplace_back(tree.root);

    while (!queue.empty()) {
      auto node = queue.back();
      queue.pop_back();

      auto label = ld.insert(node.get().label());
      string.str.push_back(static_cast<char32_t>(label));

      for (auto& children = node.get().get_children(); const auto& it : std::ranges::reverse_view(children)) {
        queue.emplace_back(it);
      }
    }
  }
};

}  // namespace ontology

#endif  // SRC_REDUCTION_HH
