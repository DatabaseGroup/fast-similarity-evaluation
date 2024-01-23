#include <variant>

#include "../data/parser.hh"
#include "../indexing/index.hh"
#include "../join/plan_execution.hh"

int main(int argc, char** argv) {
  data::StringParser string_parser;
  auto dataset = string_parser.parse(argv[1]);

  similarity::Similarity string_sim;
  string_sim = std::make_unique<similarity::SEDSimilarity>(2);

  ontology::StandardReductionGraph graph;

  auto plan_result = graph.enumerate_plans(types::DatatypeId::STRING, similarity::SimilarityId::STRING_EDIT_DISTANCE);
  auto& plans = plan_result.first;

  join::interleave_plans(dataset, string_sim, plans);

  return 0;
}
