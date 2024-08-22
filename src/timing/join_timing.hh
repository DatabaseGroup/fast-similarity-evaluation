#ifndef SRC_JOIN_TIMING_HH
#define SRC_JOIN_TIMING_HH

#include "timing.hh"

namespace timing {
class JoinTiming {
public:
  virtual ~JoinTiming() = default;

  Timer join_time;

public:
  [[nodiscard]] virtual nlohmann::json to_json() const {
    nlohmann::json json;

    join_time.add_to_json("join_time", json);

    return json;
  }
};

class TimeStaticJoinTiming : public JoinTiming {
public:
  Timer build_time;

public:
  [[nodiscard]] nlohmann::json to_json() const override {
    auto json = JoinTiming::to_json();

    build_time.add_to_json("build_time", json);

    return json;
  }
};

class TimeDynamicJoinTiming : public JoinTiming {

};

}

#endif  // SRC_JOIN_TIMING_HH
