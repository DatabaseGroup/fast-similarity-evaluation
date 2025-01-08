#ifndef HW_CACHE_HH
#define HW_CACHE_HH

#include <cstring>

namespace util {

inline volatile int64_t sum = 0;
inline volatile char* memory = nullptr;

inline void try_flush_cache() {
  // this should be larger than the last level cache; it's 1GB here
  constexpr uint64_t DOUBLE_LLC_SIZE = 1 << 30;
  constexpr uint64_t CACHELINE_SIZE = 64;
  auto local_memory = static_cast<char*>(malloc(DOUBLE_LLC_SIZE));
  memory = local_memory;
  std::memset(local_memory, 1, DOUBLE_LLC_SIZE);

  std::mt19937 prng(std::random_device{}());
  std::uniform_int_distribution<uint64_t> dist(0, DOUBLE_LLC_SIZE - 1);
  for (uint64_t i = 0; i < 32 * DOUBLE_LLC_SIZE / CACHELINE_SIZE; ++i) {
    sum += memory[dist(prng)];
  }
}

}

#endif //HW_CACHE_HH
