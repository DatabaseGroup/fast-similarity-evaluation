#ifndef ALGORITHM_RESOLUTION_HH
#define ALGORITHM_RESOLUTION_HH

#include "../ontology/planner.hh"
#include "palloc_join.hh"
#include "pass_join.hh"
#include "prefix_join.hh"
#include "result_handler.hh"
#include "t_join.hh"

namespace join {

template<class Filter = NopFilter>
std::unique_ptr<JoinAlgorithm<MaterializeHandler, Filter>> resolve_algorithmid(AlgorithmId id, similarity::Similarity& similarity) {
  switch (id) {
  case PREFIX_SIGNATURE_JOIN:
    return std::make_unique<PrefixSignatureJoin<MaterializeHandler, Filter>>(similarity);
  case FALLBACK:
    // todo implement comparing all pairs as obvious fallback
      break;
  case PASS_JOIN:
    return std::make_unique<PassJoin<MaterializeHandler, Filter>>(similarity);
  case TJOIN:
    return std::make_unique<TJoinLite<MaterializeHandler, Filter>>(similarity);
  case PALLOC:
    return std::make_unique<PallocJoin<MaterializeHandler, Filter>>(similarity);
  }
  return std::make_unique<PrefixSignatureJoin<MaterializeHandler, Filter>>(similarity);
}

}

#endif //ALGORITHM_RESOLUTION_HH
