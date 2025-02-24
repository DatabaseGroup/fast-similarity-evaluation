#ifndef SRC_HASHING_HH
#define SRC_HASHING_HH

#include <absl/random/random.h>

#include <boost/multiprecision/miller_rabin.hpp>

namespace util {

/*
uint64_t random_prime_in_range(uint64_t lower, uint64_t upper, uint32_t seed = std::random_device()()) {
  std::mt19937_64 random{seed};
  std::uniform_int_distribution dist(lower, upper);

  uint64_t n;
  do {
    n = dist(random) | 1ull;
  } while (n % 3 == 0 || n % 5 == 0 || boost::multiprecision::miller_rabin_test(n, 20));  // perform 20 tests of
primality (not too important)

  return n;
}
 */

class TabulationHash {
public:
  TabulationHash() = default;
  explicit TabulationHash(std::seed_seq seq) : bitgen(seq) {}

public:
  uint64_t get(size_t key) {
    if (key >= hashes.size()) {
      size_t old_size = hashes.size();
      hashes.reserve(key - old_size + 1);

      for (; old_size <= key; ++old_size) {
        hashes.push_back(absl::Uniform<uint64_t>(bitgen));
      }
    }
    return hashes[key];
  }

private:
  std::vector<uint64_t> hashes;
  absl::BitGen bitgen;
};

template <class INTEGER, int64_t ALPHABET = std::numeric_limits<char>::max()>
class RabinFingerprint {
public:
  explicit RabinFingerprint(int32_t window) : window(window) {
    leftmost_base = 1;
    for (int32_t i = 1; i < window; ++i) {
      leftmost_base = leftmost_base * BASE_CONSTANT;
      leftmost_base %= MODULO;
    }
  }

  void remove(INTEGER c) {
    // + MODULO to avoid underflows
    state = state + MODULO - (c * leftmost_base % MODULO);
    state %= MODULO;
  }

  uint64_t roll(INTEGER c) {
    state *= BASE_CONSTANT;
    state %= MODULO;
    state += c;
    state %= MODULO;

    return state;
  }

  [[nodiscard]] int32_t get_window() const { return window; }

  void reset() { state = 0ul; }

  [[nodiscard]] uint64_t get_state() const { return state; }

  [[nodiscard]] static constexpr int32_t used_bits() { return cilog2(MODULO); }

private:
  int32_t window;
  uint64_t state{0};
  uint64_t leftmost_base;

  // picking a prime here is reasonable
  static constexpr uint64_t BASE_CONSTANT = ALPHABET <= std::numeric_limits<char>::max() ? UINT64_C(31) : UINT64_C(53);
  // MODULO is the largest prime less than the 2^64 / maximum alphabet size
  // we need this to support multiplying the leftmost base with a value in the alphabet
  static constexpr uint64_t MODULO =
    ALPHABET <= std::numeric_limits<char>::max() ? UINT64_C(72057594037927931) : UINT64_C(4294967291);

  static constexpr int32_t cilog2(uint64_t val) {
    return val > 1 ? 1 + cilog2(val >> 1) : val == 1 ? 0 : throw std::domain_error{"cilog2(0)"};
  }
};

}  // namespace util

#endif  // SRC_HASHING_HH
