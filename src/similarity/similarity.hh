#ifndef SRC_SIMILARITY_HH
#define SRC_SIMILARITY_HH

#include <memory>
#include <numeric>
#include <variant>

#include "../types/types.hh"

namespace similarity {

enum SimilarityId { JACCARD, STRING_EDIT_DISTANCE, QGRAM_COUNT };

template <class T>
class AbstractSimilarity {
public:
  explicit AbstractSimilarity(double threshold) : threshold(threshold) {}
  virtual ~AbstractSimilarity() = default;

  virtual double similarity(const T& o1, const T& o2) = 0;
  virtual bool is_in_threshold(const T& o1, const T& o2) = 0;

public:
  double threshold;
};

class SetSimilarity : public AbstractSimilarity<types::Set> {
public:
  explicit SetSimilarity(double threshold) : AbstractSimilarity<types::Set>(threshold) {}

protected:
  virtual int64_t equivalent_overlap(int64_t s1, int64_t s2) = 0;

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
    return static_cast<int64_t>(s1.tokens.size()) -
           equivalent_overlap(static_cast<int64_t>(s1.tokens.size()), static_cast<int64_t>(s1.tokens.size())) + 1;
  }

  virtual int64_t minimum_length_bound(int64_t size) = 0;

  virtual int64_t maximum_length_bound(int64_t size) = 0;
};
using SetSimilarityPtr = std::unique_ptr<SetSimilarity>;

class StringSimilarity : public AbstractSimilarity<types::String> {
public:
  explicit StringSimilarity(double threshold) : AbstractSimilarity<types::String>(threshold) {}
};
using StringSimilarityPtr = std::unique_ptr<StringSimilarity>;

class TreeSimilarity : public AbstractSimilarity<types::Tree> {};
using TreeSimilarityPtr = std::unique_ptr<TreeSimilarity>;

class JaccardSimilarity : public SetSimilarity {
public:
  explicit JaccardSimilarity(double threshold) : SetSimilarity(threshold) {}

  double similarity(const types::Set& s1, const types::Set& s2) override {
    auto l1 = static_cast<int32_t>(s1.tokens.size());
    auto l2 = static_cast<int32_t>(s2.tokens.size());

    auto o = overlap(s1, s2);

    return 1.0 * o / (l1 + l2 - o);
  }
};

class SEDSimilarity : public StringSimilarity {
public:
  explicit SEDSimilarity(double threshold) : StringSimilarity(threshold), _thresh(static_cast<int32_t>(threshold)) {}

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
      for (int32_t j = 0; j <= std::abs(m - n) + 2*p; ++j) {
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

    return r[std::abs(m - n) + 2*p + kp + 1] <= _thresh;
  }

  [[nodiscard]] int64_t length_lower_bound(size_t string_size) const {
    return std::max(0l, static_cast<int64_t>(string_size) - _thresh);
  }

  [[nodiscard]] int64_t length_upper_bound(size_t string_size) const {
    return static_cast<int64_t>(string_size) + _thresh;
  }

private:
  const int32_t _thresh;
};

class QGramCountSimilarity : public SetSimilarity {
public:
  QGramCountSimilarity(double threshold, int32_t q)
      : SetSimilarity(threshold), q(q), integer_threshold(static_cast<int32_t>(threshold)) {}

  int64_t equivalent_overlap(int64_t s1, int64_t s2) override {
    return static_cast<int32_t>(std::max(s1, s2)) - q * integer_threshold;
  }

  double similarity(const types::Set& s1, const types::Set& s2) override { return overlap(s1, s2); }

  int64_t minimum_length_bound(int64_t size) override { return size - integer_threshold; }
  int64_t maximum_length_bound(int64_t size) override { return size + integer_threshold; }

private:
  int32_t q;
  int32_t integer_threshold;
};

using Similarity = std::variant<SetSimilarityPtr, StringSimilarityPtr, TreeSimilarityPtr>;

}  // namespace similarity

#endif  // SRC_SIMILARITY_HH
