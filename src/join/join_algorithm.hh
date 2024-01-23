#ifndef SRC_JOIN_ALGORITHM_HH
#define SRC_JOIN_ALGORITHM_HH

#include "../types/types.hh"

namespace join {

enum AlgorithmId { FALLBACK, PREFIX_SIGNATURE_JOIN };

template <class Handler>
class JoinAlgorithm {
public:
  virtual ~JoinAlgorithm() = default;

  virtual void prepare_indexing_batch(types::Batch& batch) = 0;
  virtual void prepare_probing_batch(types::Batch& batch) = 0;
  virtual void index_batch(types::Batch& batch) = 0;

  virtual void join_batch(types::Batch& batch, Handler handler) = 0;
};

}  // namespace join

#endif  // SRC_JOIN_ALGORITHM_HH
