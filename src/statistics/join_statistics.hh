#ifndef SRC_JOIN_STATISTICS_HH
#define SRC_JOIN_STATISTICS_HH

#include "statistics.hh"

namespace statistics {

struct JoinStatistics {
  CountItem<> result_size;
  CountItem<> filter_verifications;
  CountItem<> join_verifications;

  [[nodiscard]] virtual nlohmann::json to_json() const {
    nlohmann::json json;

    result_size.add_to_json("result_size", json);
    filter_verifications.add_to_json("filter_verifications", json);
    join_verifications.add_to_json("join_verifications", json);

    return json;
  }
};

}

#endif  // SRC_JOIN_STATISTICS_HH
