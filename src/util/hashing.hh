#ifndef SRC_HASHING_HH
#define SRC_HASHING_HH

#include <boost/multiprecision/miller_rabin.hpp>

namespace util {

/*
uint64_t random_prime_in_range(uint64_t lower, uint64_t upper, uint32_t seed = std::random_device()()) {
  std::mt19937_64 random{seed};
  std::uniform_int_distribution dist(lower, upper);

  uint64_t n;
  do {
    n = dist(random) | 1ull;
  } while (n % 3 == 0 || n % 5 == 0 || boost::multiprecision::miller_rabin_test(n, 20));  // perform 20 tests of primality (not too important)

  return n;
}
 */

template<class INTEGER>
class RabinFingerprint {
public:
  explicit RabinFingerprint(int32_t window)
      : window(window) {
    leftmost_base = 1;
    for (int32_t i = 1; i < window; ++i) {
      leftmost_base = leftmost_base * BASE_CONSTANT;
      leftmost_base %= MODULO;
    }
  }

  void remove(INTEGER c) {
    // + MODULO to avoid underflows
    state = state + MODULO - c * leftmost_base;
    state %= MODULO;
  }

  uint64_t roll(INTEGER c) {
    // state <= ALPHABET_SIZE / 2
    state *= BASE_CONSTANT;
    state %= MODULO;
    state += c;
    state %= MODULO;

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

  // picking a prime here is reasonable
  static constexpr uint64_t BASE_CONSTANT = 31;
  // MODULO is the largest prime less than the 2^64 / maximum alphabet size (2^32)
  // we need this to support multiplying the state with a value in the alphabet
  static constexpr uint64_t MODULO = UINT64_C(4294967291);
};

}  // namespace util

#endif  // SRC_HASHING_HH
