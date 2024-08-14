#ifndef SRC_PASS_JOIN_HH
#define SRC_PASS_JOIN_HH

#include "../similarity/similarity.hh"
#include "result_handler.hh"
#include "signature_join.hh"

namespace join {

// Used to support add_small_results in PassJoin
template <>
struct SizeGetter<std::reference_wrapper<types::String>> {
  static int64_t get_size(std::reference_wrapper<types::String>& string) {
    return static_cast<int64_t>(string.get().str.size());
  }
};

template <class Handler, class Filter = NopFilter>
class PassJoin : public SignatureJoin<Handler, Filter> {
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
    KeyIterator(int64_t probing_string_size, CachedSignatures& cached_signatures)
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
  bool has_independent_probing_signatures() override;
  std::any get_probing_signatures(types::Batch& batch) override;
  void insert_batch(types::Batch& batch) override;
  void selfjoin_batch(types::Batch& batch,
                      Handler handler,
                      statistics::JoinStatistics& statistics,
                      std::shared_ptr<std::any> probing_signatures) override;
  void join_batch(types::Batch& batch,
                  Handler handler,
                  statistics::JoinStatistics& statistics,
                  std::shared_ptr<std::any> probing_signatures) override;

private:
  template<bool IS_SELF_JOIN>
  void _join_batch(types::Batch& batch,
              std::vector<CachedSignatures>& cached_probing_signatures,
              Handler handler,
              statistics::JoinStatistics& statistics);

private:
  similarity::StringEditDistance& similarity;
  similarity::PassJoinSignature passjoin_signature;
  indexing::ComplexIndex<StringId, indexing::IndexType::ORDERED_RANDOM, indexing::IndexType::HASH> index;
  std::vector<RefString> indexed_strings;
};

template class PassJoin<MaterializeHandler, NopFilter>;
template class PassJoin<MaterializeHandler, SymmetricPairFilter>;

}  // namespace join

#endif  // SRC_PASS_JOIN_HH
