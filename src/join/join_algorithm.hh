#ifndef SRC_JOIN_ALGORITHM_HH
#define SRC_JOIN_ALGORITHM_HH

#include "../statistics/join_statistics.hh"
#include "../types/types.hh"

namespace join {

enum AlgorithmId { FALLBACK, PREFIX_SIGNATURE_JOIN, PASS_JOIN, TJOIN, PALLOC, PARTITION };

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
  case PARTITION:
    return "partition";
  }
  return "fallback";
}

inline AlgorithmId string_to_algorithm(const std::string& algorithm) {
  static const std::unordered_map<std::string, AlgorithmId> map{
    {"prefix-signature", AlgorithmId::PREFIX_SIGNATURE_JOIN},
    {"pass-join", AlgorithmId::PASS_JOIN},
    {"tjoin", AlgorithmId::TJOIN},
    {"palloc", AlgorithmId::PALLOC},
    {"partition", AlgorithmId::PARTITION}};
  auto it = map.find(algorithm);

  if (it != map.end()) {
    return it->second;
  }
  return AlgorithmId::FALLBACK;
}

enum FilterType { NOP, SIMPLE_SELFJOIN, SYMMETRIC_PAIRS, CUTOFF, CUTOFF_SELFJOIN };

struct FilterConfig {
  FilterType type;

  int64_t index_start{};
  int64_t index_end{};

  FilterConfig() : type(NOP) {}
  explicit FilterConfig(FilterType type) : type(type) {}
  FilterConfig(FilterType type, int64_t index_start, int64_t index_end)
      : type(type), index_start(index_start), index_end(index_end) {}
};

struct AbstractFilter {
  constexpr static FilterType get_filter_type() = delete;
  constexpr static bool literally_selfjoin() { return false; };

  // return true if we should skip the remainder of the current list (including this tuple)
  template <class T>
  constexpr static bool scan_break_cond([[maybe_unused]] const T& index,
                                        [[maybe_unused]] const T& probe,
                                        [[maybe_unused]] FilterConfig& config) {
    return false;
  }
  // return true if we should skip this single tuple
  template <class T>
  constexpr static bool scan_skip_cond([[maybe_unused]] const T& index,
                                       [[maybe_unused]] const T& probe,
                                       [[maybe_unused]] FilterConfig& config) {
    return false;
  }
};

struct NopFilter : AbstractFilter {
  constexpr static FilterType get_filter_type() { return NOP; }
};

struct SymmetricPairFilter : AbstractFilter {
  constexpr static FilterType get_filter_type() { return SYMMETRIC_PAIRS; }
  template <class T>
  constexpr static bool scan_skip_cond(const T& index, const T& probe, [[maybe_unused]] FilterConfig& config) {
    return !(index.id < probe.id);
  }
};

struct SimpleSelfjoinFilter : SymmetricPairFilter {
  constexpr static bool literally_selfjoin() { return true; }
};

struct CutoffFilter : AbstractFilter {
  constexpr static FilterType get_filter_type() { return CUTOFF; }

  template <class T>
  constexpr static bool scan_skip_cond(const T& index,
                                       [[maybe_unused]] const T& probe,
                                       [[maybe_unused]] FilterConfig& config) {
    return index.id < config.index_start;
  }

  template <class T>
  constexpr static bool scan_break_cond(const T& index,
                                        [[maybe_unused]] const T& probe,
                                        [[maybe_unused]] FilterConfig& config) {
    return index.id > config.index_end;
  }
};

struct CutoffSelfFilter : CutoffFilter {
  constexpr static FilterType get_filter_type() { return CUTOFF_SELFJOIN; }

  template <class T>
  constexpr static bool scan_break_cond(const T& index,
                                        [[maybe_unused]] const T& probe,
                                        [[maybe_unused]] FilterConfig& config) {
    return !(index.id < probe.id) || (index.id > config.index_end);
  }
};

template <class Handler>
class JoinAlgorithm {
public:
  virtual ~JoinAlgorithm() = default;

  virtual bool has_independent_probing_signatures() { return false; }
  virtual std::any get_probing_signatures([[maybe_unused]] types::Batch& batch) {
    throw std::invalid_argument("Cannot prepare a batch for an algorithm with dependent probing signatures.");
  }
  virtual void insert_batch(types::Batch& indexed_data, types::Batch& batch) = 0;
  virtual bool supports_merge() { return false; }
  virtual void merge([[maybe_unused]] JoinAlgorithm& o) {
    throw std::invalid_argument("Cannot merge an unmergable algorithm.");
  }

  virtual void join_batch(types::Batch& indexed_data,
                          types::Batch& batch,
                          Handler handler,
                          FilterConfig& filter_config,
                          statistics::JoinStatistics& statistics,
                          std::shared_ptr<std::any> probing_signatures) = 0;
};

}  // namespace join

#endif  // SRC_JOIN_ALGORITHM_HH
