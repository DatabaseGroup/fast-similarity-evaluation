#include "../ontology/reduction.hh"
#include "../join/signature_join.hh"
#include "../indexing/index.hh"
#include "../ontology/planner.hh"
#include "../join/plan_execution.hh"

#include <variant>

int main(int argc, char** argv) {
  types::Dataset dataset{types::Strings()};

  auto& strings = std::get<types::Strings>(dataset);
  strings.emplace_back("This is a test string");
  strings.back().id = 0;
  strings.emplace_back("This is ann test string");
  strings.back().id = 1;

  similarity::Similarity string_sim;
  string_sim = std::make_unique<similarity::SEDSimilarity>(2);

  ontology::QGramReduction qgram(4);

  auto set_sim = qgram.reduce_similarity(string_sim);
  auto set_dataset = qgram.reduce_data(dataset);

  const auto& qgram_sim = std::get<similarity::SetSimilarityPtr>(set_sim).get();

  auto& sets = std::get<types::Sets>(set_dataset);

  auto sets_are_similar = qgram_sim->is_in_threshold(sets[0], sets[1]);

  auto strings_are_similar = std::get<similarity::StringSimilarityPtr>(string_sim)->is_in_threshold(strings[0], strings[1]);

  ontology::StandardReductionGraph graph;

  auto plans = graph.enumerate_plans(types::DatatypeId::STRING, similarity::SimilarityId::STRING_EDIT_DISTANCE);

  join::execute_plan(dataset, string_sim, plans.front());

  return 0;
}
