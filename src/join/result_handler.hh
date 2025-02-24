#ifndef SRC_RESULT_HANDLER_HH
#define SRC_RESULT_HANDLER_HH

#include "../types/types.hh"
#include "../similarity/similarity.hh"

namespace join {

/*
class VerifyUntilFailureHandler {
public:
  VerifyUntilFailureHandler(types::Dataset& top_dataset,
                            similarity::Similarity& top_similarity,
                            std::vector<types::Dataset>& datasets,
                            std::vector<similarity::Similarity>& similarities,
                            std::vector<types::ResultPair>& results)
      : datasets(datasets), similarities(similarities), results(results) {}

public:
  void operator()(types::Data::Id id1, types::Data::Id id2) {
    // todo
  }

private:
  std::vector<types::Dataset>& datasets;  // in reverse order
  std::vector<similarity::Similarity>& similarities;  // in reverse order
  std::vector<types::ResultPair>& results;
};

*/

class MaterializeHandler {
public:
  explicit MaterializeHandler(std::vector<types::ResultPair>& results) : results(results) {}

  void operator()(types::Data::Id id1, types::Data::Id id2) { results.emplace_back(id1, id2); }

public:
  std::vector<types::ResultPair>& results;
};

}

#endif  // SRC_RESULT_HANDLER_HH
