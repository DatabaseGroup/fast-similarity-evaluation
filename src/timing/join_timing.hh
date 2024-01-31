#ifndef SRC_JOIN_TIMING_HH
#define SRC_JOIN_TIMING_HH

#include "timing.hh"

namespace timing {

class JoinTiming {
public:
  Timer join_time;

public:
  [[nodiscard]] nlohmann::json to_json() const {
    nlohmann::json json;

    join_time.add_to_json("join_time", json);

    return json;
  }
};

}

#endif  // SRC_JOIN_TIMING_HH
