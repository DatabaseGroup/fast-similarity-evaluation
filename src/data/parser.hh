#ifndef SRC_PARSER_HH
#define SRC_PARSER_HH

#include <fstream>

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
    std::array<char, BUF_SIZE> buffer{};

    types::Data::Id data_id = 0;
    for (std::string line; std::getline(file, line);) {
      strings.emplace_back(data_id, line);
      ++data_id;
    }
    statistics->count = static_cast<int64_t>(data_id);

    dataset.statistics = std::move(statistics);
    return dataset;
  }

private:
  static constexpr std::streamsize BUF_SIZE = 4096;
};

}  // namespace data

#endif  // SRC_PARSER_HH
