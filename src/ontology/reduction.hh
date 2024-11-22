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

inline int64_t mask_highest_bit(const uint64_t n) { return static_cast<int64_t>(n & ~(0b1uLL << 63)); }

class QGramReduction : public Reduction {
public:
  explicit QGramReduction(int32_t q) : q(q) {}
  void reduce_data(types::Batch& input_batch, types::Batch& output_batch) override {
    assert(std::holds_alternative<types::StringBatch>(input_batch) &&
           std::holds_alternative<types::SetBatch>(output_batch));
    auto& in_strings = std::get<types::StringBatch>(input_batch);
    auto& out_sets = std::get<types::SetBatch>(output_batch);
    assert(in_strings.data.size() == out_sets.data.size());
    if (in_strings.meta.alphabet_size <= std::numeric_limits<char>::max()) {
      reduce<std::numeric_limits<char>::max()>(in_strings, out_sets);
    } else {
      reduce<std::numeric_limits<types::String::char_t>::max()>(in_strings, out_sets);
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

    similarity::Similarity qgc_sim(std::make_unique<similarity::StructuralSetSimilarity>(sed.threshold, this->q));
    return qgc_sim;
  }

  [[nodiscard]] std::string get_label() const override { return std::to_string(q) + "gram"; }

protected:
  template<int64_t ALPHABET>
  void reduce(types::StringBatch& in_strings, types::SetBatch& out_sets) {
    auto in_iter = in_strings.data.begin();
    auto in_iter_end = in_strings.data.end();
    auto out_iter = out_sets.data.begin();
    // out_iter_end is reached exactly when in_iter_end is reached as both spans have the same size

    const util::RabinFingerprint<types::String::char_t, ALPHABET> rf{q};
    constexpr int32_t hash_unused_lower_bits = 63 - rf.used_bits();

    while (in_iter != in_iter_end) {
      auto& string = *in_iter;
      auto& set = *out_iter;

      set.id = string.id;
      generate_qgrams(string, set);

      std::ranges::sort(set.tokens);

      // somewhat more efficient counting of duplicates by keeping the order.
      auto last_token = set.tokens.front() - 1;
      uint64_t count = 0;
      for (auto& token : set.tokens) {
        if (token == last_token) {
          ++count;
          count &= (UINT64_C(1) << hash_unused_lower_bits) - 1;
          token += count;
        } else {
          last_token = token;
          count = 0;
        }
      }

      ++in_iter;
      ++out_iter;
    }
  }

  void generate_qgrams(types::String& string, types::Set& set) const {
    util::RabinFingerprint<types::String::char_t> rf{q};
    set.tokens.reserve(string.str.size() + q - 1);
    // highest bit has to be zero
    constexpr int32_t hash_unused_upper_bits = 63 - rf.used_bits();

    for (int32_t i = 1; i < q; ++i) {
      rf.roll(PADDING);
    }

    for (int32_t i = 0; i < static_cast<int32_t>(string.str.size()); ++i) {
      auto token = rf.roll(string.str[i]);
      token = token << hash_unused_upper_bits;
      token = mask_highest_bit(token);
      set.tokens.push_back(token);
      if (i - rf.get_window() + 1 >= 0) {
        rf.remove(string.str[i - rf.get_window() + 1]);
      } else {
        rf.remove(PADDING);
      }
    }

    for (int32_t i = q - 2; i >= 1; --i) {
      auto token = rf.roll(rf.roll(PADDING));
      token = token << hash_unused_upper_bits;
      token = mask_highest_bit(token);
      set.tokens.push_back(token);

      rf.remove(*(string.str.end() - i - 1));
    }
    if (q > 1) {
      auto token = rf.roll(rf.roll(PADDING));
      token = token << hash_unused_upper_bits;
      token = mask_highest_bit(token);
      set.tokens.push_back(token);
    }
  }

private:
  int32_t q;

  static constexpr types::String::char_t PADDING = 256;
};

class TraversalStringReduction : public Reduction {
public:
  void reduce_data(types::Batch& input_batch, types::Batch& output_batch) override {
    assert(std::holds_alternative<types::TreeBatch>(input_batch) &&
           std::holds_alternative<types::StringBatch>(output_batch));
    auto& in_trees = std::get<types::TreeBatch>(input_batch);
    auto& out_strings = std::get<types::StringBatch>(output_batch);
    assert(in_trees.data.size() == out_strings.data.size());
    out_strings.meta.alphabet_size = std::numeric_limits<types::String::char_t>::max();

    auto in_iter = in_trees.data.begin();
    auto in_iter_end = in_trees.data.end();
    auto out_iter = out_strings.data.begin();
    // out_iter_end is reached exactly when in_iter_end is reached as both spans have the same size

    while (in_iter != in_iter_end) {
      auto& tree = *in_iter;
      auto& string = *out_iter;

      string.id = tree.id;
      generate_preorder(tree, string);

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
  [[nodiscard]] std::string get_label() const override { return "traversal-strings"; }

  types::Dataset reduce_data(types::Dataset& dataset) override {
    return Reduction::forward_as_batch<types::Dataset, types::Strings>(dataset, *this);
  }

  types::Dataset reduce_data(types::Batch& input_batch) override {
    return Reduction::forward_as_batch<types::Batch, types::Strings>(input_batch, *this);
  }

private:
  static void generate_preorder(types::Tree& tree, types::String& string) {
    std::vector<std::reference_wrapper<const types::Tree::Node>> queue;
    queue.emplace_back(tree.root);

    while (!queue.empty()) {
      auto node = queue.back();
      queue.pop_back();

      auto label = std::hash<std::string>{}(node.get().label().to_string());
      string.str.push_back(static_cast<char32_t>(label ^ (label >> 32)));

      for (auto& children = node.get().get_children(); const auto& it : std::ranges::reverse_view(children)) {
        queue.emplace_back(it);
      }
    }
  }
};

class LabelSetReduction : public Reduction {
public:
  void reduce_data(types::Batch& input_batch, types::Batch& output_batch) override {
    assert(std::holds_alternative<types::TreeBatch>(input_batch) &&
           std::holds_alternative<types::SetBatch>(output_batch));
    auto& in_trees = std::get<types::TreeBatch>(input_batch);
    auto& out_sets = std::get<types::SetBatch>(output_batch);
    assert(in_trees.data.size() == out_sets.data.size());

    auto in_iter = in_trees.data.begin();
    auto in_iter_end = in_trees.data.end();
    auto out_iter = out_sets.data.begin();
    // out_iter_end is reached exactly when in_iter_end is reached as both spans have the same size

    while (in_iter != in_iter_end) {
      auto& tree = *in_iter;
      auto& set = *out_iter;

      set.id = tree.id;
      generate_labelset(tree, set);

      std::ranges::sort(set.tokens);

      // somewhat more efficient counting of duplicates by keeping the order.
      auto last_token = set.tokens.front() - 1;
      int64_t count = 0;
      for (auto& token : set.tokens) {
        if (token == last_token) {
          ++count;
          count &= (1 << counter_bits) - 1;
          token += count;
        } else {
          last_token = token;
          count = 0;
        }
      }

      ++in_iter;
      ++out_iter;
    }
  }
  similarity::Similarity reduce_similarity(similarity::Similarity& similarity) override {
    assert(std::holds_alternative<similarity::TreeSimilarityPtr>(similarity));

    // assert: Similarity is Tree Edit Distance
    auto& ted =
      dynamic_cast<similarity::TreeEditDistance&>(std::get<similarity::TreeSimilarityPtr>(similarity).operator*());

    similarity::Similarity hd(std::make_unique<similarity::StructuralSetSimilarity>(ted.threshold, 1));
    return hd;
  }

  types::Dataset reduce_data(types::Dataset& dataset) override {
    return Reduction::forward_as_batch<types::Dataset, types::Sets>(dataset, *this);
  }
  types::Dataset reduce_data(types::Batch& input_batch) override {
    return Reduction::forward_as_batch<types::Batch, types::Sets>(input_batch, *this);
  }

  [[nodiscard]] std::string get_label() const override { return "label-sets"; }

private:
  static void generate_labelset(types::Tree& tree, types::Set& set) {
    std::vector<std::reference_wrapper<const types::Tree::Node>> queue;
    queue.emplace_back(tree.root);

    while (!queue.empty()) {
      auto node = queue.back();
      queue.pop_back();

      auto token = static_cast<types::Set::Token>(std::hash<std::string>{}(node.get().label().to_string()));
      token <<= counter_bits;
      token = mask_highest_bit(token);
      set.tokens.push_back(token);

      for (auto& children = node.get().get_children(); const auto& it : children) {
        queue.emplace_back(it);
      }
    }
  }

private:
  static constexpr int64_t counter_bits = 3;
};

class JaroSetReduction : public QGramReduction {
public:
  explicit JaroSetReduction(int32_t q) : QGramReduction(q) {}

  void reduce_data(types::Batch& input_batch, types::Batch& output_batch) override {
    // First, reduce like in a standard qgram reduction
    QGramReduction::reduce_data(input_batch, output_batch);
    // Then, resolve duplicates
    auto& sets = std::get<types::SetBatch>(output_batch);
    types::HashTable<types::Set::Id, int64_t> occurrences;

    for (auto& set : sets.data) {
      for (auto& token : set.tokens) {
        token ^= static_cast<types::Set::Id>(occurrence_hash.get(occurrences[token]++));
        token &= std::numeric_limits<types::Set::Id>::max();
      }
      occurrences.clear();
      std::sort(set.tokens.begin(), set.tokens.end());
    }
  }

  similarity::Similarity reduce_similarity(similarity::Similarity& similarity) override {
    assert(std::holds_alternative<similarity::StringSimilarityPtr>(similarity));

    // assert: Similarity is Jaro Similarity
    auto& jaro =
      dynamic_cast<similarity::JaroSimilarity&>(std::get<similarity::StringSimilarityPtr>(similarity).operator*());

    similarity::Similarity ovlp(std::make_unique<similarity::JaroOverlapSimilarity>(jaro.threshold));
    return ovlp;
  }

  [[nodiscard]] std::string get_label() const override { return "jaro-set"; }

private:
  util::TabulationHash occurrence_hash;
};

}  // namespace ontology

#endif  // SRC_REDUCTION_HH
