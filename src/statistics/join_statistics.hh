#ifndef SRC_JOIN_STATISTICS_HH
#define SRC_JOIN_STATISTICS_HH

#include <utility>

#include "statistics.hh"

namespace statistics {

struct JoinStatistics {
  CountItem<> result_size;
  CountItem<> filter_verifications;
  CountItem<> join_verifications;

  virtual ~JoinStatistics() = default;

  [[nodiscard]] virtual nlohmann::json to_json() const {
    nlohmann::json json;

    result_size.add_to_json("result_size", json);
    filter_verifications.add_to_json("filter_verifications", json);
    join_verifications.add_to_json("join_verifications", json);

    return json;
  }

  JoinStatistics& operator+=(const JoinStatistics& rhs) {
    result_size.value += rhs.result_size.value;
    filter_verifications.value += rhs.filter_verifications.value;
    join_verifications.value += rhs.join_verifications.value;

    return *this;
  }

  // lhs should be copied
  friend JoinStatistics operator+(const JoinStatistics& lhs, const JoinStatistics& rhs) {
    JoinStatistics res;
    res += lhs;
    res += rhs;
    return res;
  }
};

struct LocalJoinStatistics : public JoinStatistics {
  long double bandit_weight{};
  CountItem<> selection_count;
  CountItem<> reduction_cache_hits;
  CountItem<> reduction_cache_misses;

  CountItem<> probing_signature_cache_hits;
  CountItem<> probing_signature_cache_misses;

  nlohmann::json description;

  explicit LocalJoinStatistics(nlohmann::json description) : description(std::move(description)) {}

  [[nodiscard]] nlohmann::json to_json() const override {
    auto json = JoinStatistics::to_json();
    json["bandit_weight"] = bandit_weight;
    selection_count.add_to_json("selection_count", json);
    reduction_cache_hits.add_to_json("reduction_cache_hits", json);
    reduction_cache_misses.add_to_json("reduction_cache_misses", json);
    json["reduction_cache_hitrate"] = static_cast<double>(reduction_cache_hits.value) /
                                      static_cast<double>(reduction_cache_hits.value + reduction_cache_misses.value);
    probing_signature_cache_hits.add_to_json("probing_signature_cache_hits", json);
    probing_signature_cache_misses.add_to_json("probing_signature_cache_misses", json);
    json["probing_signature_cache_hitrate"] = static_cast<double>(probing_signature_cache_hits.value) /
                                      static_cast<double>(probing_signature_cache_hits.value + probing_signature_cache_misses.value);

    json["description"] = description;
    return json;
  }
  LocalJoinStatistics& operator+=(const LocalJoinStatistics& rhs) {
    JoinStatistics::operator+=(rhs);
    bandit_weight += rhs.bandit_weight;
    selection_count.value += rhs.selection_count.value;
    return *this;
  }
  // lhs should be copied
  friend LocalJoinStatistics operator+(LocalJoinStatistics lhs, const LocalJoinStatistics& rhs) {
    lhs += rhs;
    return lhs;
  }
};

}  // namespace statistics

#endif  // SRC_JOIN_STATISTICS_HH
