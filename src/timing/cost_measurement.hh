#ifndef SRC_COST_MEASUREMENT_HH
#define SRC_COST_MEASUREMENT_HH

#include <cstdint>
#include <chrono>

namespace timing {

struct ExecutionCost;

using clock = std::chrono::steady_clock;
using cost_time_point = std::chrono::time_point<clock>;
using time_duration = std::chrono::duration<double>;
using cost_type = double;

struct ExecutionCost {
  cost_time_point point;

  ExecutionCost() = default;
  explicit ExecutionCost(cost_time_point point) : point(point) {}
};

__inline__ ExecutionCost start_cost_measurement() {
  return ExecutionCost(clock::now());
}


__inline__ ExecutionCost end_cost_measurement() {
  return ExecutionCost(clock::now());
}

__inline__ cost_type get_cost(ExecutionCost begin, ExecutionCost end) {
  time_duration duration = end.point - begin.point;
  return duration.count();
}

}

#endif  // SRC_COST_MEASUREMENT_HH
