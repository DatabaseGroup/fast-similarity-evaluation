#ifndef SRC_JOIN_ALGORITHM_HH
#define SRC_JOIN_ALGORITHM_HH

#include "../statistics/join_statistics.hh"
#include "../types/types.hh"

namespace join {

enum AlgorithmId { FALLBACK, PREFIX_SIGNATURE_JOIN, PASS_JOIN };

// there are better ways to do this, but they aren't worth it here
std::string algorithm_to_string(AlgorithmId id) {
  switch (id) {
  case FALLBACK:
    return "fallback";
  case PREFIX_SIGNATURE_JOIN:
    return "prefix-signature";
  case PASS_JOIN:
    return "pass-join";
  }
  return "fallback";
}

AlgorithmId string_to_algorithm(const std::string& algorithm) {
  static const std::unordered_map<std::string, AlgorithmId> map{
    {"prefix-signature", AlgorithmId::PREFIX_SIGNATURE_JOIN}, {"pass-join", AlgorithmId::PASS_JOIN}};
  auto it = map.find(algorithm);

  if (it != map.end()) {
    return it->second;
  }
  return AlgorithmId::FALLBACK;
}

template <class Handler>
class JoinAlgorithm {
public:
  virtual ~JoinAlgorithm() = default;

  virtual void prepare_indexing_batch(types::Batch& batch) = 0;
  virtual bool has_independent_probing_signatures() = 0;
  virtual void prepare_probing_batch(types::Batch& batch) = 0;
  virtual void index_batch(types::Batch& batch) = 0;

  // slightly misleading name:
  // this only adds set ids in one direction (i, j) if i < j
  virtual void selfjoin_batch(types::Batch& batch, Handler handler, statistics::JoinStatistics& statistics) = 0;
  virtual void join_batch(types::Batch& batch, Handler handler, statistics::JoinStatistics& statistics) = 0;
};

}  // namespace join

#endif  // SRC_JOIN_ALGORITHM_HH
