#ifndef SRC_DATASET_HH
#define SRC_DATASET_HH

#include "../types/types.hh"

namespace data {

// maybe this will be used for type-specific characteristics (length for sets and strings, depth or degree for trees)
class Statistics {
public:
  int64_t count;
};

class StringStatistics : public Statistics {};

class Dataset {
public:
  std::unique_ptr<Statistics> statistics;
  types::Dataset data;
};

}  // namespace data

#endif  // SRC_DATASET_HH
