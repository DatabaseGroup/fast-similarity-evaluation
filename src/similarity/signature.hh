#ifndef SRC_SIGNATURE_HH
#define SRC_SIGNATURE_HH

#include <boost/core/span.hpp>
#include <ranges>
#include <vector>

#include "../types/types.hh"
#include "../util/hashing.hh"

namespace similarity {

class SetQuasiSuffix {
public:
  void update_occurrences(const types::SetBatch& batch) {
    update_occurrences(batch.data.begin(), batch.data.end());
  }

  template<class It1, class It2>
  void update_occurrences(It1 begin, It2 end) {
    // take a sample of only every 4-th set (for performance reasons)
    for (; begin != end; advance_iterator_bounded(begin, end, 4)) {
      auto set = *begin;
      for (auto token : set.tokens) {
        if (occurrences[token]++ == 0) {
          all_tokens.push_back(token);
        }
      }
      total_token_count += static_cast<int64_t>(set.tokens.size());
    }
  }

  void build_token_mapping(int64_t budget = std::numeric_limits<int64_t>::max()) {
    std::vector<std::pair<types::Set::Token, int64_t>> sorted_pairs;
    sorted_pairs.reserve(occurrences.size());

    int64_t avg_token_count = total_token_count / static_cast<int64_t>(occurrences.size());

    auto begin = all_tokens.begin();
    auto end = all_tokens.end();
    for (; begin != end; advance_iterator_bounded(begin, end, 1)) {
      // only consider the tokens with at least double the occurrences count
      if (auto& entry = occurrences[*begin]; entry >= 2 * avg_token_count) {
        sorted_pairs.emplace_back(*begin, entry);
      }
    }
    std::ranges::sort(sorted_pairs, [](const auto& p1, const auto& p2) { return p1.second > p2.second; });
    auto it = sorted_pairs.begin();
    while (it != sorted_pairs.end() && budget > 0) {
      auto& [token, occ] = *it;
      heavy_tokens.emplace(token, next_token);
      --next_token;
      --budget;
      ++it;
    }
  }

  template<class It1, class It2>
  void convert_tokens(It1 begin, It2 end) {
    for (; begin != end; ++begin) {
      auto& set = *begin;

      for (auto& token : set.tokens) {
        auto it = heavy_tokens.find(token);
        if (it != heavy_tokens.end()) {
          token = it->second;
        }
      }
      std::ranges::sort(set.tokens.begin(), set.tokens.end());
    }
  }
private:
  template<class It1, class It2>
  void advance_iterator_bounded(It1& begin, It2& end, int64_t n) {
    if (std::distance(begin, end) >= n) {
      begin += n;
    } else {
      begin = end;
    }
  }

private:
  std::vector<types::Set::Token> all_tokens;
  types::HashTable<types::Set::Token, int64_t> occurrences;
  types::HashTable<types::Set::Token, types::Set::Token> heavy_tokens;
  int64_t next_token = std::numeric_limits<int64_t>::max();
  int64_t total_token_count = 0;
};

class SetPrefixSignature {
public:
  using Signature = types::Set::Token;

public:
  explicit SetPrefixSignature(SetSimilarity& similarity, SetQuasiSuffix& sqs) : similarity(similarity), sqs(sqs) {}

public:
  void update_frequencies(const types::span<types::Set> sets) const {
    if (token_budget != 0) {
      sqs.update_occurrences(sets.begin(), sets.end());
    }
  }

  void build_token_mapping() const {
    if (token_budget != 0) {
      sqs.build_token_mapping(token_budget);
    }
  }

  void convert_tokens(types::span<types::Set> sets) {
    sqs.convert_tokens(sets.begin(), sets.end());
  }

  // ReSharper disable once CppMemberFunctionMayBeStatic
  boost::span<Signature>::const_iterator begin_indexing_signatures(  // NOLINT(*-convert-member-functions-to-static)
    const types::Set& set) {
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
    auto prefix_size = similarity.probing_prefix_size(set);

    return begin_probing_signatures(set) + std::min(prefix_size, static_cast<int64_t>(set.tokens.size()));
  }

private:
  using CountOrToken = union CountOrToken {
    uint64_t count{0};
    types::Set::Token token;
  };
  types::HashTable<types::Set::Token, CountOrToken> token_map;
  const int64_t token_budget = 42000;
  SetSimilarity& similarity;
  SetQuasiSuffix& sqs;
};

class PassJoinSignature {
public:
  using Signature = uint64_t;
  using IndexingSignatures = std::vector<Signature>;

  struct CachedSignatures {
    struct LengthEntry {
      size_t begin_offset;
      size_t end_offset;

      LengthEntry(size_t begin_offset, size_t end_offset) : begin_offset(begin_offset), end_offset(end_offset) {}
    };
    std::vector<Signature> hashes;
    std::vector<LengthEntry> offsets;
  };

public:
  // we want "sufficiently random", but consistent numbers here; seeding the tabulation hash this way ensures
  // execution-specific fixed random numbers (they might be different from run to run, which is a property that we
  // actually want)
  explicit PassJoinSignature(StringEditDistance& similarity) : partition_hash(std::seed_seq{0x42424242, 0x1337}) {
    threshold = static_cast<int32_t>(similarity.threshold);
  }

public:
  // hash of partition i at [i]
  IndexingSignatures indexing_signatures(const std::u32string& string) {
    std::vector<Signature> signatures;
    int32_t offset = 0;

    for (int32_t partition = 0; partition < partition_count(); ++partition) {
      int32_t part_size = partition_size(string.size(), partition);
      util::RabinFingerprint<types::String::char_t> fp(part_size);

      for (int32_t i = offset; i < offset + part_size; ++i) {
        fp.roll(string[i]);
      }

      signatures.emplace_back(apply_partition_hash(fp.get_state(), partition));
      offset += part_size;
    }

    return signatures;
  }

  CachedSignatures cached_probing_signatures(const std::u32string& string) {
    CachedSignatures cache;
    auto string_size = static_cast<int32_t>(string.size());

    for (int64_t index_string_size = string_size - threshold; index_string_size <= string_size + threshold;
         ++index_string_size) {
      size_t length_begin = cache.hashes.size();

      int32_t offset = 0;
      for (int32_t partition = 0; partition < partition_count(); ++partition) {
        int32_t part_size = partition_size(index_string_size, partition);
        util::RabinFingerprint<types::String::char_t> fp(part_size);

        int32_t start_pos = probe_start_pos(partition, offset);
        int32_t end_pos = probe_end_pos(partition, offset, part_size, string_size);

        for (int32_t i = start_pos; i < start_pos + part_size; ++i) {
          fp.roll(string[i]);
        }
        cache.hashes.push_back(apply_partition_hash(fp.get_state(), partition));

        for (int32_t i = 0; i < (end_pos - start_pos); ++i) {
          fp.remove(string[start_pos + i]);
          auto hash = fp.roll(string[start_pos + part_size + i]);
          cache.hashes.push_back(apply_partition_hash(hash, partition));
        }

        offset += part_size;
      }

      cache.offsets.emplace_back(length_begin, cache.hashes.size());
    }

    return cache;
  }

  [[nodiscard]] int32_t partition_count() const { return threshold + 1; }

private:
  // idx is 0-indexed
  [[nodiscard]] int32_t partition_size(size_t s, int32_t idx) const {
    return static_cast<int32_t>(s) / partition_count() +
           ((static_cast<int32_t>(s) % partition_count()) >= (partition_count() - idx));
  }

  static int32_t probe_start_pos(int32_t partition_idx, int32_t partition_start) {
    return std::max(0, partition_start - partition_idx);
  }

  static int32_t probe_end_pos(int32_t partition_idx,
                               int32_t partition_start,
                               int32_t partition_length,
                               int32_t string_length) {
    return std::min(string_length - partition_length, partition_start + partition_idx);
  }

  uint64_t apply_partition_hash(uint64_t hash, int32_t partition) { return hash ^ partition_hash.get(partition); }

private:
  int32_t threshold;
  util::TabulationHash partition_hash;
};

class PallocSignature {
public:
  using Signature = uint64_t;
  struct Signatures {
    struct PartitionEntry {
      size_t begin_offset;
      size_t end_offset;

      PartitionEntry(size_t begin_offset, size_t end_offset) : begin_offset(begin_offset), end_offset(end_offset) {}
    };
    std::vector<Signature> normal_signatures;
    std::vector<Signature> deletion_signatures;
    std::vector<PartitionEntry> deletion_partition_offsets;
  };

public:
  PallocSignature()
      : partition_hash(std::seed_seq{0x42424242, 0x1337}),
        deletion_hash(util::TabulationHash(std::seed_seq{0x3133735}).get(0)) {}

  Signatures indexing_signatures(types::Set& set, int32_t partition_count, bool enable_deletion) {
    Signatures signatures;

    std::vector<int32_t> partition_size(partition_count + 1, 0);

    signatures.normal_signatures.reserve(partition_count);
    // initialize with partition-specific hash value
    for (auto i = 0; i < partition_count; ++i) {
      signatures.normal_signatures.push_back(partition_hash.get(i));
    }
    for (auto token : set.tokens) {
      auto part = partition(token, partition_count);
      signatures.normal_signatures[part] ^= hash_token(token);
      ++partition_size[part + 1];
    }
    if (enable_deletion) {
      std::partial_sum(partition_size.begin(), partition_size.end(), partition_size.begin());

      for (auto i = 0; i < partition_count; ++i) {
        signatures.deletion_partition_offsets.emplace_back(partition_size[i], partition_size[i + 1]);
      }

      signatures.deletion_signatures.resize(set.tokens.size());
      for (auto token : set.tokens) {
        auto part = partition(token, partition_count);
        signatures.deletion_signatures[partition_size[part]] =
          signatures.normal_signatures[part] ^ hash_token(token) ^ deletion_hash;
        ++partition_size[part];
      }
    }

    return signatures;
  }

  [[nodiscard]] Signature select_other_index(Signature s) const { return s ^ deletion_hash; }

private:
  static uint64_t _pseudo_fmix64(types::Set::Token token, const uint64_t c1, const uint64_t c2) {
    auto hash = static_cast<uint64_t>(token);

    hash ^= hash >> 33;
    hash *= c1;
    hash ^= hash >> 33;
    hash *= c2;
    hash ^= hash >> 33;

    return hash;
  }

  // fmix64
  static uint64_t hash_token(types::Set::Token token) {
    return _pseudo_fmix64(token, UINT64_C(0xff51afd7ed558ccd), UINT64_C(0xc4ceb9fe1a85ec53));
  }

  // some random numbers from http://zimbry.blogspot.com/2011/09/better-bit-mixing-improving-on.html
  static uint64_t partition(types::Set::Token token, int32_t partition_count) {
    auto hash = _pseudo_fmix64(token, UINT64_C(0x16a6ac37883af045), UINT64_C(0xcc9c31a4274686a5));
    return hash % partition_count;
  }

private:
  util::TabulationHash partition_hash;
  const uint64_t deletion_hash;
};

}  // namespace similarity

#endif  // SRC_SIGNATURE_HH
