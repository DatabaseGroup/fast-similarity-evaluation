#ifndef SRC_SIMILARITY_HH
#define SRC_SIMILARITY_HH

#include <tsim/node/tree_indexer.h>
#include <tsim/ted/apted_tree_index.h>
#include <tsim/ted/touzet_kr_set_tree_index.h>
#include <tsim/ted_ub/lgm_tree_index.h>

#include <numeric>
#include <variant>

#include "../types/types.hh"

namespace similarity {

enum SimilarityId {
  JACCARD,
  STRING_EDIT_DISTANCE,
  STRUCTUAL_SET_SIM,
  TREE_EDIT_DISTANCE,
  HAMMING_DISTANCE,
  JARO_OVERLAP,
  JARO_STRING
};

template <class T>
class AbstractSimilarity {
public:
  explicit AbstractSimilarity(double threshold) : threshold(threshold) {}
  virtual ~AbstractSimilarity() = default;

  virtual double similarity(const T& o1, const T& o2) = 0;
  virtual bool is_in_threshold(const T& o1, const T& o2) = 0;

  virtual int64_t always_similar_below_size([[maybe_unused]] const T& o1) { return 0; }
  virtual int64_t max_asbs() { return 0; }

public:
  double threshold;
};

class SetSimilarity : public AbstractSimilarity<types::Set> {
public:
  explicit SetSimilarity(double threshold) : AbstractSimilarity<types::Set>(threshold) {}

protected:
  static int32_t overlap(const types::Set& s1, const types::Set& s2) {
    auto it1 = s1.tokens.begin();
    auto it2 = s2.tokens.begin();

    auto end1 = s1.tokens.end();
    auto end2 = s2.tokens.end();

    int32_t ovlp = 0;

    while (it1 != end1 && it2 != end2) {
      if (*it1 == *it2) {
        ++ovlp;
        ++it1;
        ++it2;
      } else if (*it1 > *it2) {
        ++it2;
      } else {
        ++it1;
      }
    }

    return ovlp;
  }

  static bool overlap_at_least(const types::Set& r, const types::Set& s, int64_t required_ovlp) {
    assert(std::is_sorted(r.tokens.begin(), r.tokens.end()));
    assert(std::is_sorted(s.tokens.begin(), s.tokens.end()));

    auto max_r = static_cast<int64_t>(r.tokens.size());
    auto max_s = static_cast<int64_t>(s.tokens.size());

    auto iter_r = r.tokens.begin();
    auto iter_s = s.tokens.begin();
    int64_t ovlp = 0;

    while (max_r >= required_ovlp && max_s >= required_ovlp && ovlp < required_ovlp) {
      if (*iter_r == *iter_s) {
        ++iter_r;
        ++iter_s;
        ++ovlp;
      } else if (*iter_r < *iter_s) {
        ++iter_r;
        --max_r;
      } else {
        ++iter_s;
        --max_s;
      }
    }

    return ovlp >= required_ovlp;
  }

public:
  virtual int64_t equivalent_overlap(int64_t s1, int64_t s2) {
    return std::ceil(equivalent_fractional_overlap(s1, s2));
  }
  virtual double equivalent_fractional_overlap(int64_t s1, int64_t s2) = 0;
  virtual int64_t equivalent_hd(int64_t s1, int64_t s2) {
    return static_cast<int64_t>(static_cast<double>(s1 + s2) - 2 * equivalent_fractional_overlap(s1, s2));
  }
  virtual int64_t max_hd_to([[maybe_unused]] int64_t index_lower,
                            int64_t index_upper,
                            [[maybe_unused]] int64_t probe_lower,
                            int64_t probe_upper) {
    // by default, this almost always defaults to inserting the upper bound in the equivalent hd due to monotonicity
    return equivalent_hd(index_upper, probe_upper);
  }
  virtual int64_t max_hd_to([[maybe_unused]] int64_t index_lower, int64_t index_upper, int64_t probing) {
    // by default, this almost always defaults to inserting the upper bound in the equivalent hd due to monotonicity
    return max_hd_to(index_lower, index_upper, probing, probing);
  }

  double similarity(const types::Set& s1, const types::Set& s2) override = 0;

  bool is_in_threshold(const types::Set& s1, const types::Set& s2) override {
    return overlap_at_least(
      s1, s2, equivalent_overlap(static_cast<int64_t>(s1.tokens.size()), static_cast<int64_t>(s2.tokens.size())));
  }

  int64_t indexing_prefix_size(const types::Set& s1) {
    auto size = static_cast<int64_t>(s1.tokens.size());
    return size - equivalent_overlap(minimum_length_bound(size), size) + 1;
  }

  int64_t probing_prefix_size(const types::Set& s1) {
    auto size = static_cast<int64_t>(s1.tokens.size());
    return size - equivalent_overlap(minimum_length_bound(size), size) + 1;
  }

  virtual int64_t minimum_length_bound(int64_t size) = 0;

  virtual int64_t maximum_length_bound(int64_t size) = 0;

  virtual int64_t maximum_length_pel(int64_t size, [[maybe_unused]] int64_t position) {
    return maximum_length_bound(size);
  }
};
using SetSimilarityPtr = std::unique_ptr<SetSimilarity>;

class StringSimilarity : public AbstractSimilarity<types::String> {
public:
  explicit StringSimilarity(double threshold) : AbstractSimilarity<types::String>(threshold) {}
};
using StringSimilarityPtr = std::unique_ptr<StringSimilarity>;

class TreeSimilarity : public AbstractSimilarity<types::Tree> {
public:
  explicit TreeSimilarity(double threshold) : AbstractSimilarity(threshold) {}
};
using TreeSimilarityPtr = std::unique_ptr<TreeSimilarity>;

using Similarity = std::variant<SetSimilarityPtr, StringSimilarityPtr, TreeSimilarityPtr>;

class JaccardSimilarity : public SetSimilarity {
public:
  explicit JaccardSimilarity(double threshold) : SetSimilarity(threshold) {}

  double similarity(const types::Set& s1, const types::Set& s2) override {
    auto l1 = static_cast<int32_t>(s1.tokens.size());
    auto l2 = static_cast<int32_t>(s2.tokens.size());

    auto o = overlap(s1, s2);

    return 1.0 * o / (l1 + l2 - o);
  }

  double equivalent_fractional_overlap(int64_t s1, int64_t s2) override {
    return (threshold / (1 + threshold)) * static_cast<double>((s1 + s2));
  }
  int64_t minimum_length_bound(int64_t size) override { return std::ceil(static_cast<double>(size) * threshold); }
  int64_t maximum_length_bound(int64_t size) override { return std::floor(static_cast<double>(size) / threshold); }
  int64_t maximum_length_pel(int64_t size, int64_t position) override {
    return std::floor((static_cast<double>(size) - (1 + threshold) * static_cast<double>(position)) / threshold);
  };
};

class StringEditDistance : public StringSimilarity {
public:
  explicit StringEditDistance(double threshold)
      : StringSimilarity(threshold), _thresh(static_cast<int32_t>(threshold)) {}

  double similarity(const types::String& s1, const types::String& s2) override {
    auto& str1 = s1.str.size() <= s2.str.size() ? s1.str : s2.str;
    auto& str2 = s1.str.size() <= s2.str.size() ? s2.str : s1.str;

    std::vector<int32_t> column1(str1.size() + 1, 0);
    std::vector<int32_t> column2(str1.size() + 1, 0);
    std::iota(column1.begin(), column1.end(), 0);

    for (auto c2 : str2) {
      column2[0] = column1[0] + 1;

      for (size_t i = 1; i <= str1.size(); ++i) {
        auto c1 = str1[i - 1];
        column2[i] = std::min(column2[i - 1] + 1, std::min(column1[i] + 1, column1[i - 1] + (c1 != c2)));
      }

      std::swap(column1, column2);
    }

    return column1.back();
  }

  bool is_in_threshold(const types::String& s1, const types::String& s2) override {
    // this assumes that ||s1| - |s2|| <= threshold
    assert(std::abs(static_cast<int32_t>(s1.str.size()) - static_cast<int32_t>(s2.str.size())) <= _thresh);
    auto& b = s1.str.size() <= s2.str.size() ? s1.str : s2.str;
    auto& a = s1.str.size() <= s2.str.size() ? s2.str : s1.str;
    auto n = static_cast<int32_t>(b.size());
    auto m = static_cast<int32_t>(a.size());

    int32_t p = std::floor((_thresh - (std::abs(m - n))) / 2.0);
    int kp, k;
    kp = k = n >= m ? -p : -p + n - m;

    // r_-1 == r[0]
    // r_0 == r[1] etc.
    // r_|n-m| + 2p + 1 == r[|n-m|+2p + 2]
    // --> |n-m|+2p + 3 entries, initially "infinity"
    std::vector<int32_t> r(std::abs(m - n) + 2 * p + 3, std::numeric_limits<int32_t>::max() / 2);

    for (int32_t i = 0; i <= m; ++i) {
      for (int32_t j = 0; j <= std::abs(m - n) + 2 * p; ++j) {
        if (i == 0 && i == j + k) {
          r[j + 1] = 0;
        } else if (i == 0) {
          r[j + 1] = r[j] + 1;
        } else {
          auto ca = a[i - 1];
          auto cb = 0 <= j + k - 1 && j + k - 1 < n ? b[j + k - 1] : '\0';
          r[j + 1] = std::min(r[j + 1] + (ca != cb), std::min(r[j + 2] + 1, r[j] + 1));
        }
      }
      ++k;
    }

    return r[std::abs(m - n) + 2 * p + kp + 1] <= _thresh;
  }

  [[nodiscard]] int64_t minimum_length_bound(size_t string_size) const {
    return std::max(INT64_C(0), static_cast<int64_t>(string_size) - _thresh);
  }

  [[nodiscard]] int64_t maximum_length_bound(size_t string_size) const {
    return static_cast<int64_t>(string_size) + _thresh;
  }

  int64_t always_similar_below_size(const types::String& o1) override {
    if (static_cast<int32_t>(o1.str.size()) < _thresh) {
      return std::max(_thresh - static_cast<int64_t>(o1.str.size()), INT64_C(0));
    }
    return 0;
  }

  int64_t max_asbs() override { return _thresh; };

private:
  const int32_t _thresh;
};

// == the "set edit distance"; number of tokens to insert, delete, or replace to make both sets equal
class StructuralSetSimilarity : public SetSimilarity {
public:
  StructuralSetSimilarity(double threshold, int32_t q)
      : SetSimilarity(threshold), q(q), integer_threshold(static_cast<int32_t>(threshold)) {}

  double equivalent_fractional_overlap(int64_t s1, int64_t s2) override {
    return static_cast<int32_t>(std::max(s1, s2)) - q * integer_threshold;
  }

  int64_t max_hd_to(int64_t index_lower, int64_t index_upper, int64_t probe_lower, int64_t probe_upper) override {
    if (probe_upper < index_lower) {
      return equivalent_hd(index_lower, probe_upper);
    } else if (index_upper < probe_lower) {
      return equivalent_hd(index_upper, probe_lower);
    } else {
      return equivalent_hd(probe_upper, probe_upper);
    }
  }

  double similarity(const types::Set& s1, const types::Set& s2) override { return overlap(s1, s2); }

  int64_t minimum_length_bound(int64_t size) override { return size - integer_threshold; }
  int64_t maximum_length_bound(int64_t size) override { return size + integer_threshold; }

  int64_t always_similar_below_size(const types::Set& o1) override {
    if (static_cast<int64_t>(o1.tokens.size()) <= q * integer_threshold) {
      return q * integer_threshold;
    }
    return 0;
  }

  int64_t max_asbs() override { return q * integer_threshold; }

  int64_t maximum_length_pel(int64_t size, int64_t position) override {
    return std::min(size + integer_threshold, size + integer_threshold - position / q);
  }

private:
  int32_t q;
  int32_t integer_threshold;
};

class HammingDistance : public SetSimilarity {
public:
  explicit HammingDistance(double threshold)
      : SetSimilarity(threshold), integer_threshold(static_cast<int32_t>(threshold)) {}

  int64_t minimum_length_bound(int64_t size) override { return std::max(INT64_C(0), size - integer_threshold); }
  int64_t maximum_length_bound(int64_t size) override { return size + integer_threshold; }

  int64_t always_similar_below_size(const types::Set& o1) override {
    auto set_size = static_cast<int64_t>(o1.tokens.size());
    if (set_size < integer_threshold) {
      return integer_threshold - set_size;
    }
    return 0;
  }

  int64_t max_asbs() override { return integer_threshold; };

  double similarity(const types::Set& s1, const types::Set& s2) override {
    return static_cast<double>(s1.tokens.size() + s2.tokens.size() - 2 * overlap(s1, s2));
  }

  // todo pel for Hamming

protected:
  double equivalent_fractional_overlap(int64_t s1, int64_t s2) override {
    return static_cast<double>(s1 + s2 - integer_threshold) / 2;
  }

private:
  int32_t integer_threshold;
};

class TreeEditDistance : public TreeSimilarity {
public:
  TreeEditDistance(const double threshold,
                   types::Tree::LabelDictionary& label_dictionary,
                   types::Tree::CostModel& cost_model)
      : TreeSimilarity(threshold),
        integer_threshold(static_cast<int32_t>(threshold)),
        label_dictionary(label_dictionary),
        cost_model(cost_model),
        apted(cost_model),
        touzet(cost_model),
        lgm_algorithm(cost_model) {}

  double similarity(const types::Tree& o1, const types::Tree& o2) override {
    tsim::node::TreeIndexAPTED t1;
    tsim::node::TreeIndexAPTED t2;
    index_tree(t1, o1.root, label_dictionary, cost_model);
    index_tree(t2, o2.root, label_dictionary, cost_model);

    return apted.ted(t1, t2);
  }
  bool is_in_threshold(const types::Tree& o1, const types::Tree& o2) override {
    {
      tsim::node::TreeIndexLGM ti_1;
      tsim::node::TreeIndexLGM ti_2;
      index_tree(ti_1, o1.root, label_dictionary, cost_model);
      index_tree(ti_2, o2.root, label_dictionary, cost_model);

      double ubted = lgm_algorithm.ted_k(ti_1, ti_2, integer_threshold);

      if (ubted <= integer_threshold) {
        return true;
      }
    }

    tsim::node::TreeIndexTouzetKRSet t1;
    tsim::node::TreeIndexTouzetKRSet t2;
    index_tree(t1, o1.root, label_dictionary, cost_model);
    index_tree(t2, o2.root, label_dictionary, cost_model);

    return touzet.ted_k(t1, t2, integer_threshold) <= threshold;
  }

private:
  const int32_t integer_threshold;
  types::Tree::LabelDictionary& label_dictionary;
  types::Tree::CostModel& cost_model;
  tsim::ted::APTEDTreeIndex<types::Tree::CostModel> apted;
  tsim::ted::TouzetKRSetTreeIndex<types::Tree::CostModel> touzet;
  tsim::ted_ub::LGMTreeIndex<types::Tree::CostModel> lgm_algorithm;
};

class JaroOverlapSimilarity : public SetSimilarity {
public:
  explicit JaroOverlapSimilarity(double threshold) : SetSimilarity(threshold) {}

public:
  double similarity(const types::Set& o1, const types::Set& o2) override {
    auto ovlp = static_cast<double>(overlap(o1, o2));
    return 1. / 3. * (ovlp / static_cast<double>(o1.tokens.size()) + ovlp / static_cast<double>(o2.tokens.size()) + 1.);
  }
  double equivalent_fractional_overlap(int64_t s1, int64_t s2) override {
    return (3 * threshold - 1) * static_cast<double>(s1 * s2) / static_cast<double>(s1 + s2);
  }
  int64_t minimum_length_bound(int64_t size) override {
    return std::max(INT64_C(0), static_cast<int64_t>(std::ceil(static_cast<double>(size) * (3 * threshold - 2))));
  }
  int64_t maximum_length_bound(int64_t size) override {
    return static_cast<int64_t>(std::floor(static_cast<double>(size) / (3 * threshold - 2)));
  }
  int64_t maximum_length_pel(int64_t size, int64_t position) override {
    return std::floor(static_cast<double>(size * size - position * size) /
                      (static_cast<double>(position) + static_cast<double>(size) * (3 * threshold - 2)));
  }
  int64_t max_hd_to(int64_t index_lower, int64_t index_upper, int64_t probe_lower, int64_t probe_upper) override {
    return equivalent_hd(std::min(index_lower, probe_lower), std::max(index_upper, probe_upper));
  }
};

class JaroSimilarity : public StringSimilarity {
public:
  explicit JaroSimilarity(double threshold) : StringSimilarity(threshold) {}

public:
  double similarity(const types::String& o1, const types::String& o2) override { return jaro(o1.str, o2.str, 0); }
  bool is_in_threshold(const types::String& o1, const types::String& o2) override {
    return jaro(o1.str, o2.str, threshold) >= threshold;
  }

private:
  static int get_transpositions(const types::String::str_t& match_string,
                                const std::vector<bool>& matched_in_query_string,
                                const std::vector<types::String::char_t>& common_chars) {
    int32_t curr_common_char_pos{};
    int32_t transpositions{};

    for (int32_t i = 0; i < static_cast<int32_t>(match_string.length()); i++) {
      if (matched_in_query_string[i]) {
        if (match_string[i] != common_chars[curr_common_char_pos]) {
          transpositions++;
        }
        curr_common_char_pos++;

        if (curr_common_char_pos >= static_cast<int32_t>(common_chars.size())) {
          break;
        }
      }
    }

    return transpositions;
  }

  static double compute_jaro(int32_t matches, int32_t transpositions, int32_t string_length1, int32_t string_length2) {
    if (matches == 0)
      return 0.0;

    return (static_cast<double>(matches) / static_cast<double>(string_length1) +
            static_cast<double>(matches) / static_cast<double>(string_length2) +
            (static_cast<double>(matches) - static_cast<double>(transpositions) / 2) / static_cast<double>(matches)) /
           3;
  }

  static int jaro_lookup_matches(int query_length, int index_length, double threshold) {
    return ceil(query_length * index_length * (3 * threshold - 1) / (query_length + index_length));
  }

  static double jaro(const types::String::str_t& query_string,
                     const types::String::str_t& match_string,
                     double jaro_threshold) {
    const auto query_string_length = static_cast<int32_t>(query_string.length());
    const auto match_string_length = static_cast<int32_t>(match_string.length());
    const auto max_char_distance = std::max(std::max(query_string_length, match_string_length) / 2 - 1, 0);
    std::vector matched_match(match_string.length(), false);
    int max_query_matches;
    std::vector<types::String::char_t> common_chars{};
    common_chars.reserve(match_string_length);

    int requiredMatches = jaro_lookup_matches(query_string_length, match_string_length, jaro_threshold);
    int matches{};

    for (int queryPos = 0; queryPos < query_string_length; queryPos++)  // query string
    {
      max_query_matches = query_string_length - queryPos + matches;
      if (max_query_matches < requiredMatches) {
        return 0.0;
      }

      types::String::char_t curr_char_query = query_string[queryPos];
      int jaro_window_end = std::min(queryPos + max_char_distance, match_string_length - 1);
      for (int match_pos = std::max(0, queryPos - max_char_distance); match_pos <= jaro_window_end;
           match_pos++) {  // lookup string
        if (curr_char_query == match_string[match_pos] && !matched_match[match_pos]) {
          matched_match[match_pos] = true;
          matches++;
          common_chars.push_back(curr_char_query);
          break;
        }
      }
    }
    int transpositions = get_transpositions(match_string, matched_match, common_chars);
    return compute_jaro(matches, transpositions, query_string_length, match_string_length);
  }
};

}  // namespace similarity

#endif  // SRC_SIMILARITY_HH
