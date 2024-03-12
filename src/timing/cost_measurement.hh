#ifndef SRC_COST_MEASUREMENT_HH
#define SRC_COST_MEASUREMENT_HH

#include <cstdint>
#include <chrono>

namespace timing {

struct ExecutionCost;

#if defined(__aarch64__) || defined(_M_ARM64)
using clock = std::chrono::steady_clock;
using cost_time_point = std::chrono::time_point<clock>;
using time_duration = std::chrono::duration<double>;
using cost_type = double;

struct ExecutionCost {
  cost_time_point point;

  explicit ExecutionCost(cost_time_point point) : point(point) {}
};
#else
using cost_type = uint64_t;

struct ExecutionCost {
  uint64_t ticks;

  explicit ExecutionCost(uint64_t ticks) : ticks(ticks) {}
};
#endif

__inline__ ExecutionCost start_cost_measurement() {
#if defined(__aarch64__) || defined(_M_ARM64)
  return ExecutionCost(clock::now());
#else
  unsigned cycles_low, cycles_high;
  asm volatile ("CPUID\n\t"
               "RDTSC\n\t"
               "mov %%edx, %0\n\t"
               "mov %%eax, %1\n\t": "=r" (cycles_high), "=r" (cycles_low)::
                                                          "%rax", "%rbx", "%rcx", "%rdx");
  uint64_t ticks = ((uint64_t)cycles_high << 32) | cycles_low;
  return ExecutionCost(ticks);
#endif
}


__inline__ ExecutionCost end_cost_measurement() {
#if defined(__aarch64__) || defined(_M_ARM64)
  return ExecutionCost(clock::now());
#else
  unsigned cycles_low, cycles_high;
  asm volatile("RDTSCP\n\t"
               "mov %%edx, %0\n\t"
               "mov %%eax, %1\n\t"
               "CPUID\n\t": "=r" (cycles_high), "=r" (cycles_low):: "%rax",
                             "%rbx", "%rcx", "%rdx");
  uint64_t ticks = ((uint64_t)cycles_high << 32) | cycles_low;
  return ExecutionCost(ticks);
#endif
}

__inline__ cost_type get_cost(ExecutionCost begin, ExecutionCost end) {
#if defined(__aarch64__) || defined(_M_ARM64)
  time_duration duration = end.point - begin.point;
  return duration.count();
#else
  return end.ticks - begin.ticks;
#endif
}

}

#endif  // SRC_COST_MEASUREMENT_HH
