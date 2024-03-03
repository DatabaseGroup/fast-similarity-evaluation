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
  void prepare_index(types::SetBatch& sets) {
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

    prepare_probe(sets);
  }

  void prepare_probe(types::SetBatch& sets) {
    for (auto& set : sets) {
      // take reference on token to modify it directly
      for (auto& token : set.tokens) {
        auto it = token_map.find(token);

        if (it != token_map.end()) {
          token = it->second.token;
        } else {
          // token 0 symbolizes non-existence (minimum "real" token value is 1)
          token = 0;
        }
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

    return begin_indexing_signatures(set) + std::min(prefix_size, static_cast<int64_t>(set.tokens.size()));
  }

  boost::span<Signature>::const_iterator end_probing_signatures(const types::Set& set) {
    auto prefix_size = similarity.indexing_prefix_size(set);

    return begin_probing_signatures(set) + std::min(prefix_size, static_cast<int64_t>(set.tokens.size()));
  }

private:
  using CountOrToken = union CountOrToken {
    uint64_t count{0};
    types::Set::Token token;
  };
  absl::flat_hash_map<types::Set::Token, CountOrToken> token_map;
  similarity::SetSimilarity& similarity;
};

class PassJoinSignature {
public:
  struct Signature {
    Signature(size_t partition, size_t hash) : partition(partition), hash(hash) {}
    size_t partition;
    size_t hash;
  };

public:
  explicit PassJoinSignature(SEDSimilarity& similarity) : similarity(similarity) {
    threshold = static_cast<int32_t>(similarity.threshold);
  }

public:
  std::vector<Signature> indexing_signatures(std::string& string) {
    std::vector<Signature> signatures;
    int32_t offset = 0;

    for (int32_t partition = 0; partition < partition_count(); ++partition) {
      int32_t part_size = partition_size(string.size(), partition);
      util::RabinFingerprint fp(part_size);

      for (size_t i = offset; i < offset + part_size; ++i) {
        fp.roll(string[i]);
      }

      signatures.emplace_back(partition, fp.get_state());
      offset += part_size;
    }

    return signatures;
  }

  std::vector<Signature> probing_signatures(std::string& string) {
    std::vector<Signature> signatures;
    int32_t offset = 0;
    auto string_size = static_cast<int32_t>(string.size());

    for (int32_t partition = 0; partition < partition_count(); ++partition) {
      int32_t part_size = partition_size(string.size(), partition);
      util::RabinFingerprint fp(part_size);

      int32_t start_pos = probe_start_pos(partition, offset, part_size, string_size);
      int32_t end_pos = probe_end_pos(partition, offset, part_size, string_size);

      for (int32_t i = start_pos; i < start_pos + part_size; ++i) {
        fp.roll(string[i]);
      }
      signatures.emplace_back(partition, fp.get_state());

      for (int32_t i = 0; i < (end_pos - start_pos); ++i) {
        fp.remove(string[start_pos + i]);
        auto hash = fp.roll(string[start_pos + part_size + i]);
        signatures.emplace_back(partition, hash);
      }

      offset += part_size;
    }

    return signatures;
  }

  [[nodiscard]] int32_t partition_count() const {
    return threshold + 1;
  }

private:
  // idx is 0 indexed
  int32_t partition_size(size_t s, int32_t idx) {
    return static_cast<int32_t>(s) / partition_count() + ((static_cast<int32_t>(s) % partition_count()) >= (partition_count() - idx));
  }

  int32_t probe_start_pos(int32_t partition_idx, int32_t partition_start, [[maybe_unused]] int32_t partition_length, [[maybe_unused]] int32_t string_length) {
    return std::max(0, partition_start - partition_idx);
  }

  int32_t probe_end_pos(int32_t partition_idx, int32_t partition_start, int32_t partition_length, int32_t string_length) {
    return std::min(string_length - partition_length, partition_start + partition_idx);
  }

private:
  similarity::SEDSimilarity& similarity;
  int32_t threshold;
};

}  // namespace similarity

#endif  // SRC_SIGNATURE_HH
