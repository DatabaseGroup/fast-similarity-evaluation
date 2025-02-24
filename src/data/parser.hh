#ifndef SRC_PARSER_HH
#define SRC_PARSER_HH

#include <fstream>
#include <tsim/parser/bracket_notation_parser.h>

#include "dataset.hh"

namespace data {

class Parser {
public:
  virtual ~Parser() = default;

public:
  virtual Dataset parse(const std::string& filename) {
    return parse_until(filename, std::numeric_limits<int64_t>::max());
  }
  virtual Dataset parse_until(const std::string& filename, int64_t line_number) = 0;
};

class SetParser : public Parser {
public:
  Dataset parse_until(const std::string& filename, int64_t line_number) override {
    Dataset dataset;
    dataset.data = types::Sets{};

    auto statistics = std::make_unique<SetStatistics>();
    auto& sets = std::get<types::Sets>(dataset.data);

    std::ifstream file(filename);

    types::Data::Id data_id = 0;
    for (std::string line; std::getline(file, line) && line_number > 0;) {
      std::stringstream integers(line);
      auto& s = sets.data.emplace_back(data_id);
      while (integers.good() && !integers.eof()) {
        types::Set::Token token;
        integers >> token;
        s.tokens.push_back(token);
      }
      ++data_id;
      --line_number;
    }
    statistics->count = data_id;

    dataset.statistics = std::move(statistics);
    return dataset;
  }
};

class StringParser : public Parser {
public:
  Dataset parse_until(const std::string& filename, int64_t line_number) override {
    Dataset dataset;
    dataset.data = types::Strings{};
    auto statistics = std::make_unique<StringStatistics>();
    auto& strings = std::get<types::Strings>(dataset.data);
    strings.meta.alphabet_size = std::numeric_limits<char>::max();  // Assume 1 byte for each character

    std::ifstream file(filename);

    types::Data::Id data_id = 0;
    for (std::string line; std::getline(file, line) && line_number > 0;) {
      strings.data.emplace_back(data_id, std::u32string(line.begin(), line.end()));
      ++data_id;
      --line_number;
    }
    statistics->count = data_id;

    dataset.statistics = std::move(statistics);
    return dataset;
  }
};

class TreeParser : public Parser {
public:
  Dataset parse_until(const std::string& filename, int64_t line_number) override {
    Dataset dataset;
    dataset.data.emplace<types::Trees>();
    auto statistics = std::make_unique<TreeStatistics>();
    auto& trees = std::get<types::Trees>(dataset.data);

    tsim::parser::BracketNotationParser<types::Tree::Label> parser;
    std::ifstream trees_file(filename);

    types::Data::Id data_id = 0;
    for (std::string line; std::getline(trees_file, line) && line_number > 0;) {
      if (!parser.validate_input(line)) {
        continue;
      }
      trees.data.emplace_back(data_id, parser.parse_single(line));
      ++data_id;
      --line_number;
    }
    statistics->count = data_id;

    dataset.statistics = std::move(statistics);
    return dataset;
  }
};

}  // namespace data

#endif  // SRC_PARSER_HH
