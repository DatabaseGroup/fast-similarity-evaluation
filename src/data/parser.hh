#ifndef SRC_PARSER_HH
#define SRC_PARSER_HH

#include <fstream>
#include <tsim/parser/bracket_notation_parser.h>

#include "dataset.hh"

namespace data {

class StringParser {
public:
  Dataset parse(const std::string& filename) {
    Dataset dataset;
    dataset.data = types::Strings{};
    auto statistics = std::make_unique<StringStatistics>();
    auto& strings = std::get<types::Strings>(dataset.data);

    std::ifstream file(filename);

    types::Data::Id data_id = 0;
    for (std::string line; std::getline(file, line);) {
      strings.data.emplace_back(data_id, std::u32string(line.begin(), line.end()));
      ++data_id;
    }
    statistics->count = data_id;

    dataset.statistics = std::move(statistics);
    return dataset;
  }
};

class TreeParser {
public:
  Dataset parse(const std::string& filename) {
    Dataset dataset;
    dataset.data.emplace<types::Trees>();
    auto statistics = std::make_unique<TreeStatistics>();
    auto& trees = std::get<types::Trees>(dataset.data);

    tsim::parser::BracketNotationParser<types::Tree::Label> parser;
    std::ifstream trees_file(filename);

    types::Data::Id data_id = 0;
    for (std::string line; std::getline(trees_file, line);) {
      if (!parser.validate_input(line)) {
        continue;
      }
      trees.data.emplace_back(data_id, parser.parse_single(line));
      ++data_id;
    }
    statistics->count = data_id;

    dataset.statistics = std::move(statistics);
    return dataset;
  }
};

}  // namespace data

#endif  // SRC_PARSER_HH
