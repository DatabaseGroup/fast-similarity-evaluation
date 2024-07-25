#ifndef ALGORITHM_RESOLUTION_HH
#define ALGORITHM_RESOLUTION_HH

#include "../ontology/planner.hh"
#include "palloc_join.hh"
#include "pass_join.hh"
#include "prefix_join.hh"
#include "result_handler.hh"
#include "t_join.hh"

namespace join {

std::unique_ptr<JoinAlgorithm<MaterializeHandler>> resolve_algorithmid(AlgorithmId id, similarity::Similarity& similarity) {
  switch (id) {
  case PREFIX_SIGNATURE_JOIN:
    return std::make_unique<PrefixSignatureJoin<MaterializeHandler>>(similarity);
  case FALLBACK:
    // todo implement comparing all pairs as obvious fallback
      break;
  case PASS_JOIN:
    return std::make_unique<PassJoin<MaterializeHandler>>(similarity);
  case TJOIN:
    return std::make_unique<TJoinLite<MaterializeHandler>>(similarity);
  case PALLOC:
    return std::make_unique<PallocJoin<MaterializeHandler>>(similarity);
  }
  return std::make_unique<PrefixSignatureJoin<MaterializeHandler>>(similarity);
}

}

#endif //ALGORITHM_RESOLUTION_HH
