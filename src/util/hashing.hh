#ifndef SRC_HASHING_HH
#define SRC_HASHING_HH

#include <boost/multiprecision/miller_rabin.hpp>

namespace util {

uint64_t random_prime_in_range(uint64_t lower, uint64_t upper) {
  std::mt19937_64 random{std::random_device()()};

  uint64_t n;

  do {
    n = random() | 1ull;
  } while (boost::multiprecision::miller_rabin_test(n, 10));  // perform 10 tests of primality (not too important)

  return n;
}

class RabinFingerprint {
public:
  explicit RabinFingerprint(int32_t window)
      : window(window) {
    leftmost_base = 1;
    for (int32_t i = 1; i < window; ++i) {
      leftmost_base = leftmost_base * ALPHABET_SIZE;
    }
  }

  void remove(uint8_t c) {
    state = state - c * leftmost_base;
  }

  uint64_t roll(uint8_t c) {
    state *= ALPHABET_SIZE;
    state += c;

    return state;
  }

  [[nodiscard]] int32_t get_window() const {
    return window;
  }

  void reset() {
    state = 0ul;
  }

  [[nodiscard]] uint64_t get_state() const {
    return state;
  }

private:
  int32_t window;
  uint64_t state{0};
  uint64_t leftmost_base;

  static const uint64_t ALPHABET_SIZE = 255;
};

}  // namespace util

#endif  // SRC_HASHING_HH
