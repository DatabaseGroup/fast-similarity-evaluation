#include "../ontology/reduction.hh"
#include "../join/signature_join.hh"
#include "../indexing/index.hh"

#include <variant>

int main(int argc, char** argv) {
  types::Dataset dataset{types::Strings()};

  auto& strings = std::get<types::Strings>(dataset);
  strings.emplace_back("This is a test string");
  strings.emplace_back("This is annn test string");

  similarity::Similarity string_sim;
  string_sim = std::make_unique<similarity::SEDSimilarity>(2);

  ontology::QGramReduction qgram(4);

  auto set_sim = qgram.reduce_similarity(string_sim);
  auto set_dataset = qgram.reduce_data(dataset);

  const auto& qgram_sim = std::get<similarity::SetSimilarityPtr>(set_sim).get();

  auto& sets = std::get<types::Sets>(set_dataset);

  auto sets_are_similar = qgram_sim->is_in_threshold(sets[0], sets[1]);

  auto strings_are_similar = std::get<similarity::StringSimilarityPtr>(string_sim)->is_in_threshold(strings[0], strings[1]);

  join::PrefixSignatureJoin prefix_join(set_sim);

  prefix_join.prepare_dataset(set_dataset);
  prefix_join.index_dataset(set_dataset);
  prefix_join.join_dataset(set_dataset, [](auto& s1, auto& s2) {
    std::cout << "Found set pair" << std::endl;
  });

  return 0;
}
