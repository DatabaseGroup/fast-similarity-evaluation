#ifndef SRC_SIGNATURE_HH
#define SRC_SIGNATURE_HH

#include <vector>
#include <boost/core/span.hpp>

#include "../types/types.hh"
#include "../indexing/index.hh"

namespace similarity {

class SetPrefixSignature {
public:
  using Signature = int64_t;

public:
  explicit SetPrefixSignature(similarity::SetSimilarity& similarity) : similarity(similarity) {}

public:
  void prepare(types::Sets& sets) {
    for (auto& set : sets) {
      for (auto token : set.tokens) {
        ++token_map[token].count;
      }
    }

    using TokenCountPair = std::pair<types::Set::Token, uint64_t>;
    std::vector<TokenCountPair> entries;
    entries.reserve(token_map.size());

    for (auto& entry : token_map) {
      entries.emplace_back(entry.first, entry.second.count);
    }

    std::sort(entries.begin(), entries.end(), [](const TokenCountPair& o1, const TokenCountPair& o2){
      return o1.second < o2.second;
    });

    int64_t current_token = 1;
    for (auto& entry : entries) {
      token_map[entry.first].token = current_token;
      ++current_token;
    }

    for (auto& set : sets) {
      // take reference on token to modify it directly
      for (auto& token : set.tokens) {
        token = token_map[token].token;
      }
      std::sort(set.tokens.begin(), set.tokens.end());
    }

    std::sort(sets.begin(), sets.end(), [](const types::Set& s1, const types::Set& s2){
      if (s1.tokens.size() != s2.tokens.size()) {
        return s1.tokens.size() < s2.tokens.size();
      }
      return std::lexicographical_compare(s1.tokens.begin(), s1.tokens.end(), s2.tokens.begin(), s2.tokens.end());
    });
  }

  boost::span<Signature>::const_iterator begin_indexing_signatures(const types::Set& set) {
    return &(*set.tokens.begin());
  }

  boost::span<Signature>::const_iterator begin_probing_signatures(const types::Set& set) {
    return begin_indexing_signatures(set);
  }

  boost::span<Signature>::const_iterator end_indexing_signatures(const types::Set& set) {
    auto prefix_size = similarity.indexing_prefix_size(set);

    return begin_indexing_signatures(set) + prefix_size;
  }

  boost::span<Signature>::const_iterator end_probing_signatures(const types::Set& set) {
    auto prefix_size = similarity.indexing_prefix_size(set);

    return begin_probing_signatures(set) + prefix_size;
  }

private:
  using CountOrToken = union CountOrToken {
    uint64_t count{0};
    types::Set::Token token;
  };
  absl::flat_hash_map<types::Set::Token, CountOrToken> token_map;
  similarity::SetSimilarity& similarity;
};

}  // namespace similarity

#endif  // SRC_SIGNATURE_HH
