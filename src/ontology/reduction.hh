#ifndef SRC_REDUCTION_HH
#define SRC_REDUCTION_HH

#include "../similarity/similarity.hh"
#include "../types/types.hh"
#include "../util/hashing.hh"

namespace ontology {

class Reduction {
public:
  virtual ~Reduction() = default;

public:
  // Assert: input data and output data have the same ids
  virtual types::Dataset reduce_data(types::Dataset& data) = 0;
  virtual similarity::Similarity reduce_similarity(similarity::Similarity& similarity) = 0;
};

class QGramReduction : public Reduction {
public:
  explicit QGramReduction(int32_t q) : q(q) {}
  types::Dataset reduce_data(types::Dataset& data) override {
    assert(std::holds_alternative<types::Strings>(data));

    auto& strings = std::get<types::Strings>(data);

    types::Dataset qgrams{types::Sets()};
    auto sets = std::get<types::Sets>(qgrams);

    for (auto& string : strings) {
      auto& new_set = sets.emplace_back(string.id);
      generate_qgrams(string, new_set);
    }

    return sets;
  }

  similarity::Similarity reduce_similarity(similarity::Similarity& similarity) override {
    assert(std::holds_alternative<similarity::StringSimilarityPtr>(similarity));

    // assert: Similarity is String Edit Distance
    auto& sed =
      dynamic_cast<similarity::SEDSimilarity&>(std::get<similarity::StringSimilarityPtr>(similarity).operator*());

    similarity::Similarity qgc_sim(std::make_unique<similarity::QGramCountSimilarity>(sed.threshold, this->q));
    return qgc_sim;
  }

private:
  void generate_qgrams(types::String& string, types::Set& set) const {
    util::RabinFingerprint rf{q};
    set.tokens.reserve(string.str.size() + q - 1);

    for (int32_t i = 1; i < q; ++i) {
      rf.roll(PADDING);
    }

    for (int32_t i = 0; i < static_cast<int32_t>(string.str.size()); ++i) {
      uint64_t token = rf.roll(string.str[i]);
      set.tokens.push_back(token);
      if (i - rf.get_window() + 1 >= 0) {
        rf.remove(string.str[i - rf.get_window() + 1]);
      } else {
        rf.remove(PADDING);
      }
    }

    for (int32_t i = q - 2; i >= 1; --i) {
      uint64_t token = rf.roll(PADDING);
      set.tokens.push_back(token);

      rf.remove(*(string.str.end() - i - 1));
    }
    uint64_t token = rf.roll(PADDING);
    set.tokens.push_back(token);
  }

private:
  int32_t q;

  static const uint8_t PADDING =
    127;  // 127 is not a printable ASCII character (it is DEL), it hopefully does not/cannot appear in text
};

}  // namespace ontology

#endif  // SRC_REDUCTION_HH
