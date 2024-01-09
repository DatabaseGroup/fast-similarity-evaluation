#include <variant>

#include "../data/parser.hh"
#include "../indexing/index.hh"
#include "../join/plan_execution.hh"
#include "../join/signature_join.hh"
#include "../ontology/planner.hh"
#include "../ontology/reduction.hh"

int main(int argc, char** argv) {
  data::StringParser string_parser;
  auto dataset = string_parser.parse(argv[1]);

  similarity::Similarity string_sim;
  string_sim = std::make_unique<similarity::SEDSimilarity>(2);

  ontology::StandardReductionGraph graph;

  auto plans = graph.enumerate_plans(types::DatatypeId::STRING, similarity::SimilarityId::STRING_EDIT_DISTANCE);

  join::execute_plan(dataset.data, string_sim, plans.front());

  return 0;
}
