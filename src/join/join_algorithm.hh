#ifndef SRC_JOIN_ALGORITHM_HH
#define SRC_JOIN_ALGORITHM_HH

#include "../statistics/join_statistics.hh"
#include "../types/types.hh"

namespace join {

enum AlgorithmId { FALLBACK, PREFIX_SIGNATURE_JOIN, PASS_JOIN, TJOIN, PALLOC };

// there are better ways to do this, but they aren't worth it here
inline std::string algorithm_to_string(AlgorithmId id) {
  switch (id) {
  case FALLBACK:
    return "fallback";
  case PREFIX_SIGNATURE_JOIN:
    return "prefix-signature";
  case PASS_JOIN:
    return "pass-join";
  case TJOIN:
    return "tjoin";
  case PALLOC:
    return "palloc";
  }
  return "fallback";
}

inline AlgorithmId string_to_algorithm(const std::string& algorithm) {
  static const std::unordered_map<std::string, AlgorithmId> map{
    {"prefix-signature", AlgorithmId::PREFIX_SIGNATURE_JOIN},
    {"pass-join", AlgorithmId::PASS_JOIN},
    {"tjoin", AlgorithmId::TJOIN},
    {"palloc", AlgorithmId::PALLOC}};
  auto it = map.find(algorithm);

  if (it != map.end()) {
    return it->second;
  }
  return AlgorithmId::FALLBACK;
}

struct NopFilter {
  static bool set_pred([[maybe_unused]] types::Set& index_set, [[maybe_unused]] types::Set& probe_set) { return true; }
  static bool string_pred([[maybe_unused]] types::String& index_string, [[maybe_unused]] types::String& probe_string) {
    return true;
  }
  static bool tree_pred([[maybe_unused]] types::Tree& index_tree, [[maybe_unused]] types::Tree& probe_tree) {
    return true;
  }
};

struct SymmetricPairFilter {
  static bool set_pred(const types::Set& index_set, const types::Set& probe_set) { return index_set.id < probe_set.id; }
  static bool string_pred(const types::String& index_string, const types::String& probe_string) {
    return index_string.id < probe_string.id;
  }
  static bool tree_pred(const types::Tree& index_tree, const types::Tree& probe_tree) {
    return index_tree.id < probe_tree.id;
  }
};

template <class Handler, class Filter = NopFilter>
class JoinAlgorithm {
public:
  virtual ~JoinAlgorithm() = default;

  virtual void prepare_indexing_batch(types::Batch& batch) = 0;
  virtual bool has_independent_probing_signatures() { return false; }
  virtual std::any prepare_probing_batch([[maybe_unused]] types::Batch& batch) {
    throw std::invalid_argument("Cannot prepare a batch for an algorithm with dependent probing signatures.");
  }
  virtual void index_batch(types::Batch& batch) = 0;

  virtual void selfjoin_batch(types::Batch& batch,
                              Handler handler,
                              statistics::JoinStatistics& statistics,
                              std::shared_ptr<std::any> probing_signatures) = 0;
  virtual void join_batch(types::Batch& batch,
                          Handler handler,
                          statistics::JoinStatistics& statistics,
                          std::shared_ptr<std::any> probing_signatures) = 0;
};

}  // namespace join

#endif  // SRC_JOIN_ALGORITHM_HH
