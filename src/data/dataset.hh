#ifndef SRC_DATASET_HH
#define SRC_DATASET_HH

#include <nlohmann/json.hpp>
#include "../types/types.hh"

namespace data {

// maybe this will be used for type-specific characteristics (length for sets and strings, depth or degree for trees)
class Statistics {
public:
  int64_t count;
  virtual ~Statistics() = default;

  virtual nlohmann::json to_json() {
    nlohmann::json json;

    json["count"] = count;

    return json;
  }
};

class SetStatistics : public Statistics {};
class StringStatistics : public Statistics {};
class TreeStatistics : public Statistics {};

class Dataset {
public:
  std::unique_ptr<Statistics> statistics;
  types::Dataset data;

  Dataset() = default;

  Dataset(Dataset&& other) noexcept = default;
  Dataset& operator=(Dataset&& other) noexcept {
    statistics = std::move(other.statistics);
    data = std::move(other.data);
    return *this;
  };
};

}  // namespace data

#endif  // SRC_DATASET_HH
