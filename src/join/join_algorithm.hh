#ifndef SRC_JOIN_ALGORITHM_HH
#define SRC_JOIN_ALGORITHM_HH

#include "../types/types.hh"
#include "../statistics/join_statistics.hh"

namespace join {

enum AlgorithmId { FALLBACK, PREFIX_SIGNATURE_JOIN };

template <class Handler>
class JoinAlgorithm {
public:
  virtual ~JoinAlgorithm() = default;

  virtual void prepare_indexing_batch(types::Batch& batch) = 0;
  virtual void prepare_probing_batch(types::Batch& batch) = 0;
  virtual void index_batch(types::Batch& batch) = 0;

  virtual void join_batch(types::Batch& batch, Handler handler, statistics::JoinStatistics& statistics) = 0;

  // slightly misleading name:
  // this only adds set ids in one direction (i, j) if i < j
  virtual void selfjoin_batch(types::Batch& batch, Handler handler, statistics::JoinStatistics& statistics) = 0;
};

}  // namespace join

#endif  // SRC_JOIN_ALGORITHM_HH
