#ifndef SRC_HASHING_HH
#define SRC_HASHING_HH

#include <absl/numeric/int128.h>
#include <absl/random/random.h>

#include <boost/multiprecision/miller_rabin.hpp>
#include <cstdint>
#include <limits>
#include <type_traits>

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

// INTEGER is an at most 32-bit integer type
template <class INTEGER>
class RabinFingerprint {
  static_assert(std::is_unsigned_v<INTEGER> && std::numeric_limits<INTEGER>::digits <= 32);

public:
  explicit RabinFingerprint(const int32_t window) : window(window) {
    leftmost_base = 1;
    for (int32_t i = 1; i < window; ++i) {
      leftmost_base = reduce(absl::uint128{leftmost_base} * BASE_CONSTANT);
    }
  }

  void remove(INTEGER c) {
    // c is a at most 32 bit integer, leftmost_base is a 64 bit (actually 56 due to reduction) integer
    // after multiplication, we need at most 96 bits
    // use a uint128 for the multiplication to avoid overflows
    const uint64_t removed = reduce(absl::uint128{c} * leftmost_base);
    state = state >= removed ? state - removed : state + MODULO - removed;
  }

  uint64_t roll(INTEGER c) {
    state = reduce((absl::uint128{state} * BASE_CONSTANT) + static_cast<uint64_t>(c));
    return state;
  }

  [[nodiscard]] int32_t get_window() const { return window; }

  void reset() { state = 0ul; }

  [[nodiscard]] uint64_t get_state() const { return state; }

  [[nodiscard]] static constexpr int32_t used_bits() { return FINGERPRINT_BITS; }

private:
  // perform mod reduction as described in https://arxiv.org/abs/2008.08654 (using a "near-mersenne" prime)
  static uint64_t reduce(absl::uint128 x) {
    const uint64_t lo = absl::Uint128Low64(x) & MASK;
    const uint64_t hi = absl::Uint128Low64(x >> FINGERPRINT_BITS);

    uint64_t r = lo + UINT64_C(5) * hi;

    if (r >= MODULO) {
      r -= MODULO;
    }

    return r;
  }

private:
  int32_t window;
  uint64_t state{0};
  uint64_t leftmost_base;

  static constexpr int32_t FINGERPRINT_BITS = 56;
  static constexpr uint64_t MASK = (UINT64_C(1) << FINGERPRINT_BITS) - 1;
  static constexpr uint64_t MODULO = (UINT64_C(1) << FINGERPRINT_BITS) - 5;  // a "near-mersenne" prime
  static constexpr uint64_t BASE_CONSTANT = (UINT64_C(1) << 32) + 2;  // smallest primitive root for MODULO above 2^32
};

}  // namespace util

#endif  // SRC_HASHING_HH
