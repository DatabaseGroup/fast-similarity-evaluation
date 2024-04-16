#ifndef SRC_SIGNATURE_HH
#define SRC_SIGNATURE_HH

#include <boost/core/span.hpp>
#include <vector>
#include <ranges>

#include "../types/types.hh"
#include "../util/hashing.hh"

namespace similarity {

class SetPrefixSignature {
public:
  using Signature = int64_t;

public:
  explicit SetPrefixSignature(similarity::SetSimilarity& similarity) : similarity(similarity) {}

public:
  void prepare_index(std::vector<types::Set>& sets) {
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

    std::ranges::sort(entries,
                      [](const TokenCountPair& o1, const TokenCountPair& o2) { return o1.second < o2.second; });

    int64_t current_token = 1;
    for (auto& key : entries | std::views::keys) {
      token_map[key].token = current_token;
      ++current_token;
    }

    prepare_probe(sets);
  }

  void prepare_probe(std::vector<types::Set>& sets) {
    for (auto& set : sets) {
      // take reference on token to modify it directly
      for (auto& token : set.tokens) {
        if (auto it = token_map.find(token); it != token_map.end()) {
          token = it->second.token;
        } else {
          // token 0 symbolizes non-existence (minimum "real" token value is 1)
          token = 0;
        }
      }
      std::ranges::sort(set.tokens);
    }

    std::sort(sets.begin(), sets.end(), [](const types::Set& s1, const types::Set& s2) {
      return s1.tokens.size() < s2.tokens.size();
    });
  }

  boost::span<Signature>::const_iterator begin_indexing_signatures(
    const types::Set& set) {  // NOLINT(*-convert-member-functions-to-static)
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
  absl::flat_hash_map<types::Set::Token, CountOrToken> token_map;
  similarity::SetSimilarity& similarity;
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
      util::RabinFingerprint<std::u32string::value_type> fp(part_size);

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
        util::RabinFingerprint<std::u32string::value_type> fp(part_size);

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
  PallocSignature() : partition_hash(std::seed_seq{0x42424242, 0x1337}),
                      deletion_hash(
                        // util::TabulationHash(std::seed_seq{0x3133735}).get(0)
                        0
                        ) {}

  Signatures indexing_signatures(types::Set& set, int32_t partition_count) {
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
    std::partial_sum(partition_size.begin(), partition_size.end(), partition_size.begin());

    for (auto i = 0; i < partition_count; ++i) {
      signatures.deletion_partition_offsets.emplace_back(partition_size[i], partition_size[i + 1]);
    }

    signatures.deletion_signatures.resize(set.tokens.size());
    for (auto token : set.tokens) {
      auto part = partition(token, partition_count);
      signatures.deletion_signatures[partition_size[part]] = signatures.normal_signatures[part] ^ hash_token(token) ^ deletion_hash;
      ++partition_size[part];
    }

    return signatures;
  }

  Signature select_other_index(Signature s) {
    return s ^ deletion_hash;
  }

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
