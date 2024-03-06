#ifndef SRC_JOIN_STATISTICS_HH
#define SRC_JOIN_STATISTICS_HH

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
  long double bandit_weight;
  CountItem<> selection_count;
  nlohmann::json description;

  explicit LocalJoinStatistics(const nlohmann::json& description) : description(description) {}

  nlohmann::json to_json() const override {
    auto json = JoinStatistics::to_json();
    json["bandit_weight"] = bandit_weight;
    selection_count.add_to_json("selection_count", json);
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

}

#endif  // SRC_JOIN_STATISTICS_HH
