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
  void update_occurences(const types::SetBatch& batch) {
    update_occurences(batch.data.begin(), batch.data.end());
  }

  template<class It1, class It2>
  void update_occurences(It1 begin, It2 end) {
    for (; begin != end; ++begin) {
      auto set = *begin;
      for (auto token : set.tokens) {
        ++occurences[token];
      }
    }
  }

  void build_token_mapping(int64_t budget = std::numeric_limits<int64_t>::max()) {
    std::vector<std::pair<types::Set::Token, int64_t>> sorted_pairs;
    sorted_pairs.reserve(occurences.size());
    for (auto& entry : occurences) {
      sorted_pairs.emplace_back(entry);
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

  std::vector<types::Set> convert_tokens(const types::SetBatch& batch) {
    return convert_tokens(batch.data.begin(), batch.data.end());
  }

  template<class It1, class It2>
  std::vector<types::Set> convert_tokens(It1 begin, It2 end) {
    std::vector<types::Set> sets;
    sets.reserve(std::distance(begin, end));

    for (; begin != end; ++begin) {
      auto& set = *begin;
      auto& new_set = sets.emplace_back(set.id);

      for (auto token : set.tokens) {
        // assert: no "real-world" dataset (i.e., not converted by reduction) has tokens with values between 2^62 and 2^63
        auto it = heavy_tokens.find(token);

        if (it != heavy_tokens.end()) {
          new_set.tokens.push_back(it->second);
        } else {
          // if real world dataset: this AND does not change anything
          // if reduced dataset: might add some false positives, but those are filtered on the other datatypes anyway
          new_set.tokens.push_back(static_cast<int64_t>(static_cast<uint64_t>(token) & (~(UINT64_C(11) << 62))));
        }
      }

      std::ranges::sort(new_set.tokens);
    }

    return sets;
  }

private:
  types::HashTable<types::Set::Token, int64_t> occurences;
  types::HashTable<types::Set::Token, types::Set::Token> heavy_tokens;
  int64_t next_token = std::numeric_limits<int64_t>::max();
};

class SetPrefixSignature {
public:
  using Signature = int64_t;

public:
  explicit SetPrefixSignature(SetSimilarity& similarity) : similarity(similarity) {}

public:
  void prepare_index(std::vector<types::Set>& sets) {
    sqs.update_occurences(sets.begin(), sets.end());
    sqs.build_token_mapping(42000);

    prepare_probe(sets);
  }

  void prepare_probe(std::vector<types::Set>& sets) {
    sets = sqs.convert_tokens(sets.begin(), sets.end());

    std::sort(sets.begin(), sets.end(), [](const types::Set& s1, const types::Set& s2) {
      return s1.tokens.size() < s2.tokens.size();
    });
  }

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
  absl::flat_hash_map<types::Set::Token, CountOrToken> token_map;
  SetSimilarity& similarity;
  SetQuasiSuffix sqs;
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
  PallocSignature()
      : partition_hash(std::seed_seq{0x42424242, 0x1337}),
        deletion_hash(util::TabulationHash(std::seed_seq{0x3133735}).get(0)) {}

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
      signatures.deletion_signatures[partition_size[part]] =
        signatures.normal_signatures[part] ^ hash_token(token) ^ deletion_hash;
      ++partition_size[part];
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
