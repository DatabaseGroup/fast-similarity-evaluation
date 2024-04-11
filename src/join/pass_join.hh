#ifndef SRC_PASS_JOIN_HH
#define SRC_PASS_JOIN_HH

#include "../similarity/similarity.hh"
#include "signature_join.hh"

namespace join {

// Used to support add_small_results in PassJoin
template <>
struct SizeGetter<std::reference_wrapper<types::String>> {
  static int64_t get_size(std::reference_wrapper<types::String>& string) {
    return static_cast<int64_t>(string.get().str.size());
  }
};

template <class Handler>
class PassJoin : public SignatureJoin<Handler> {
private:
  using StringId = int64_t;
  using RefString = std::reference_wrapper<types::String>;

public:
  using CachedSignatures = similarity::PassJoinSignature::CachedSignatures;

private:
  class KeyIterator {
    template <int32_t LEVEL, class DUMMY = void>
    struct IteratorHolder {};

    // string size level
    template <class DUMMY>
    struct IteratorHolder<0, DUMMY> {
      using iter = types::span<similarity::PassJoinSignature::Signature>::iterator;

      static void set_level_key(KeyIterator& iterator, indexing::KeyType key) {
        iterator.index_string_size = key;
        int64_t size_diff = iterator.index_string_size - iterator.probing_string_size + iterator.epsilon;
        auto& length_entry = iterator.cached_signatures.offsets[size_diff];
        auto& hashes = iterator.cached_signatures.hashes;

        iterator.current_signatures = types::span<similarity::PassJoinSignature::Signature>(
          hashes.begin() + length_entry.begin_offset, hashes.begin() + length_entry.end_offset);
      }

      static iter get_level_iterator(KeyIterator& iterator) { return iterator.current_signatures.begin(); }

      static iter get_level_end(KeyIterator& iterator) { return iterator.current_signatures.end(); }
    };

  public:
    explicit KeyIterator(int64_t probing_string_size, CachedSignatures& cached_signatures)
        : probing_string_size(probing_string_size),
          cached_signatures(cached_signatures),
          epsilon((static_cast<int64_t>(cached_signatures.offsets.size()) - 1) / 2) {}

  public:
    template <int32_t LEVEL>
    void set_level_key([[maybe_unused]] indexing::KeyType key) {
      IteratorHolder<LEVEL>::set_level_key(*this, key);
    }

    template <int32_t LEVEL>
    typename IteratorHolder<LEVEL>::iter get_level_iterator() {
      return IteratorHolder<LEVEL>::get_level_iterator(*this);
    }

    template <int32_t LEVEL>
    typename IteratorHolder<LEVEL>::iter get_level_end() {
      return IteratorHolder<LEVEL>::get_level_end(*this);
    }

  public:
    const int64_t probing_string_size;
    int64_t index_string_size{0};
    types::span<similarity::PassJoinSignature::Signature> current_signatures;
    CachedSignatures& cached_signatures;
    int64_t epsilon;
  };

public:
  // assume PassJoin gets a SEDSimilarity (nothing else works anyway)
  explicit PassJoin(similarity::Similarity& similarity)
      : similarity(
          dynamic_cast<similarity::StringEditDistance&>(*std::get<similarity::StringSimilarityPtr>(similarity))),
        passjoin_signature(this->similarity) {}

public:
  void prepare_indexing_batch(types::Batch& batch) override {
    auto& strings = std::get<types::StringBatch>(batch);

    indexed_strings.clear();
    indexed_strings.reserve(strings.data.size());
    indexed_strings.insert(indexed_strings.begin(), strings.data.begin(), strings.data.end());

    std::sort(indexed_strings.begin(), indexed_strings.end(), [](RefString& s1, RefString& s2) {
      auto& str1 = s1.get().str;
      auto& str2 = s2.get().str;
      if (str1.size() != str2.size()) {
        return str1.size() < str2.size();
      }
      return std::ranges::lexicographical_compare(str1, str2);
    });
  }

  void index_batch([[maybe_unused]] types::Batch& batch) override {
    // strings (or their references) already in indexed_strings
    // assert indexed_strings == batch (up to the order)

    // this is a wrapped reference == pointer, do not take reference

    int64_t id = 0;
    for (auto string_ref : indexed_strings) {
      auto& string = string_ref.get().str;

      for (auto signatures = passjoin_signature.indexing_signatures(string); const auto sig : signatures) {
        index.insert(id, static_cast<int64_t>(string.size()), sig);
      }

      ++id;
    }
  }

  bool has_independent_probing_signatures() override { return true; }

  std::any prepare_probing_batch([[maybe_unused]] types::Batch& batch) override {
    auto strings = std::get<types::StringBatch>(batch);

    std::vector<CachedSignatures> signatures;
    signatures.reserve(strings.data.size());

    for (auto& string : strings.data) {
      signatures.emplace_back(passjoin_signature.cached_probing_signatures(string.str));
    }

    return signatures;
  }

  void join_batch(types::Batch& batch,
                  Handler handler,
                  statistics::JoinStatistics& statistics,
                  std::shared_ptr<std::any> probing_signatures) override {
    if (probing_signatures->has_value()) {
      auto& signatures = std::any_cast<std::vector<CachedSignatures>&>(*probing_signatures);
      _join_batch<false>(batch, signatures, handler, statistics);
    } else {
      auto signatures = std::any_cast<std::vector<CachedSignatures>>(prepare_probing_batch(batch));
      _join_batch<false>(batch, signatures, handler, statistics);
    }
  }

  void selfjoin_batch(types::Batch& batch,
                      Handler handler,
                      statistics::JoinStatistics& statistics,
                      std::shared_ptr<std::any> probing_signatures) override {
    if (probing_signatures->has_value()) {
      auto& signatures = std::any_cast<std::vector<CachedSignatures>&>(*probing_signatures);
      _join_batch<true>(batch, signatures, handler, statistics);
    } else {
      auto signatures = std::any_cast<std::vector<CachedSignatures>>(prepare_probing_batch(batch));
      _join_batch<true>(batch, signatures, handler, statistics);
    }
  }

  template <bool IS_SELF_JOIN>
  void _join_batch(types::Batch& batch,
                   std::vector<CachedSignatures>& cached_probing_signatures,
                   Handler handler,
                   statistics::JoinStatistics& statistics) {
    auto strings = std::get<types::StringBatch>(batch);

    std::vector<bool> already_seen(indexed_strings.size());
    std::vector<StringId> candidates;

    for (size_t i = 0; i < strings.data.size(); ++i) {
      auto& string = strings.data[i];
      auto& probing_signatures = cached_probing_signatures[i];

      KeyIterator key_iterator(static_cast<int64_t>(string.str.size()), probing_signatures);

      int64_t minimum_candidate_size = similarity.minimum_length_bound(string.str.size());
      int64_t maximum_candidate_size = similarity.maximum_length_bound(string.str.size());

      index.query(
        indexing::KeyRange(minimum_candidate_size, maximum_candidate_size),
        [&](StringId set_id) {
          if (!already_seen[set_id]) {
            already_seen[set_id] = true;
            candidates.push_back(set_id);
          }
        },
        key_iterator);

      // candidate_id != candidate_set.id
      // set_id and candidate_id are internal to the join implementation only
      for (auto candidate_id : candidates) {
        // set from indexed data (indexed_sets set in index_batch)
        auto candidate_string = indexed_strings[candidate_id];

        if constexpr (IS_SELF_JOIN) {
          if (string.id <= candidate_string.get().id) {
            already_seen[candidate_id] = false;
            continue;
          }
        }

        if (similarity.is_in_threshold(candidate_string, string)) {
          handler(candidate_string.get().id, string.id);
        }

        already_seen[candidate_id] = false;
      }

      statistics.join_verifications.add(static_cast<int64_t>(candidates.size()));
      candidates.clear();
    }
  }

private:
  similarity::StringEditDistance& similarity;
  similarity::PassJoinSignature passjoin_signature;
  indexing::ComplexIndex<StringId, indexing::IndexType::ORDERED, indexing::IndexType::HASH> index;
  std::vector<RefString> indexed_strings;
};

}  // namespace join

#endif  // SRC_PASS_JOIN_HH
