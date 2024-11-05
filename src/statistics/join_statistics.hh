#ifndef SRC_JOIN_STATISTICS_HH
#define SRC_JOIN_STATISTICS_HH

#include <utility>

#include "join_statistics.hh"
#include "statistics.hh"

namespace statistics {

struct ReductionCacheStatistics {
  CountItem<> reduction_cache_hits;
  CountItem<> reduction_cache_misses;

  CountItem<> probing_signature_cache_hits;
  CountItem<> probing_signature_cache_misses;

  [[nodiscard]] nlohmann::json to_json() const {
    nlohmann::json json;
    reduction_cache_hits.add_to_json("reduction_cache_hits", json);
    reduction_cache_misses.add_to_json("reduction_cache_misses", json);
    json["reduction_cache_hitrate"] = static_cast<double>(reduction_cache_hits.value) /
                                      static_cast<double>(reduction_cache_hits.value + reduction_cache_misses.value);
    probing_signature_cache_hits.add_to_json("probing_signature_cache_hits", json);
    probing_signature_cache_misses.add_to_json("probing_signature_cache_misses", json);
    json["probing_signature_cache_hitrate"] =
      static_cast<double>(probing_signature_cache_hits.value) /
      static_cast<double>(probing_signature_cache_hits.value + probing_signature_cache_misses.value);

    return json;
  }
};

struct JoinStatistics {
  CountItem<> result_size;
  CountItem<> join_verifications;
  CountItem<> index_skips;
  double indexed_ratio{};

  virtual ~JoinStatistics() = default;

  [[nodiscard]] virtual nlohmann::json to_json() const {
    nlohmann::json json;

    result_size.add_to_json("result_size", json);
    join_verifications.add_to_json("join_verifications", json);
    index_skips.add_to_json("index_skips", json);
    json["indexed_ratio"] = indexed_ratio;

    return json;
  }
};

struct LocalJoinStatistics : JoinStatistics {
  ReductionCacheStatistics rc_statistics;
  AvgFloatItem<> bandit_weight;
  std::vector<CountItem<>> step_verifications;
  CountItem<> selection_count;
  nlohmann::json description;

  explicit LocalJoinStatistics(nlohmann::json description) : description(std::move(description)) {}

  [[nodiscard]] nlohmann::json to_json() const override {
    auto json = JoinStatistics::to_json();
    json["reduction_cache"] = rc_statistics.to_json();
    json["bandit_weights"] = bandit_weight.avg();

    std::vector verifications{join_verifications.value};
    std::for_each(
      step_verifications.begin(), step_verifications.end(), [&](auto& cnt) { verifications.push_back(cnt.value); });
    verifications.push_back(result_size.value);
    json["intermediary_sizes"] = verifications;

    selection_count.add_to_json("selection_count", json);

    json["description"] = description;
    return json;
  }
};

struct LocalBlockSliceStatistics : LocalJoinStatistics {
  long double incurred_loss{};
  long double expected_total_loss{std::numeric_limits<long double>::infinity()};

  explicit LocalBlockSliceStatistics(const nlohmann::json& description) : LocalJoinStatistics(description) {}

  [[nodiscard]] nlohmann::json to_json() const override {
    auto json = LocalJoinStatistics::to_json();
    json["incurred_loss"] = incurred_loss;
    json["expected_total_loss"] = expected_total_loss;

    json["description"] = description;
    return json;
  }
};

struct LocalTimeSliceStatistics : LocalJoinStatistics {
  explicit LocalTimeSliceStatistics(const nlohmann::json& description) : LocalJoinStatistics(description) {}
};

struct LocalDynamicTimeSliceStatistics : LocalJoinStatistics {
  explicit LocalDynamicTimeSliceStatistics(const nlohmann::json& description) : LocalJoinStatistics(description) {}

  CountItem<> index_cache_hits;
  CountItem<> index_cache_misses;

  [[nodiscard]] nlohmann::json to_json() const override {
    auto json = LocalJoinStatistics::to_json();
    index_cache_hits.add_to_json("index_cache_hits", json);
    index_cache_misses.add_to_json("index_cache_misses", json);

    return json;
  }
};

struct GlobalJoinStatistics : JoinStatistics {
  virtual void merge(LocalJoinStatistics& local_stat) {
    this->result_size.value += local_stat.result_size.value;
    this->join_verifications.value += local_stat.join_verifications.value;
    this->indexed_ratio += local_stat.indexed_ratio;
  }
};

struct GlobalBlockSliceStatistics : GlobalJoinStatistics {
  long double incurred_loss{};

  void merge(LocalJoinStatistics& foreign_stat) override {
    GlobalJoinStatistics::merge(foreign_stat);

    auto& local_stat = dynamic_cast<LocalBlockSliceStatistics&>(foreign_stat);
    this->incurred_loss += local_stat.incurred_loss;
  }

  [[nodiscard]] nlohmann::json to_json() const override {
    auto json = GlobalJoinStatistics::to_json();
    json["incurred_loss"] = incurred_loss;
    return json;
  }
};

struct GlobalTimeSliceStatistics : GlobalJoinStatistics {};

struct GlobalDynamicTimeSliceStatistics : GlobalJoinStatistics {
  CountItem<> index_cache_hits;
  CountItem<> index_cache_misses;

  void merge(LocalJoinStatistics &foreign_stat) override {
    GlobalJoinStatistics::merge(foreign_stat);

    auto& local_stat = dynamic_cast<LocalDynamicTimeSliceStatistics&>(foreign_stat);
    this->index_cache_hits.value += local_stat.index_cache_hits.value;
    this->index_cache_misses.value += local_stat.index_cache_misses.value;
  }

  [[nodiscard]] nlohmann::json to_json() const override {
    auto json = GlobalJoinStatistics::to_json();
    index_cache_hits.add_to_json("index_cache_hits", json);
    index_cache_misses.add_to_json("index_cache_misses", json);

    return json;
  }
};

using LocalStatistics = std::vector<std::unique_ptr<LocalJoinStatistics>>;

inline void merge_local_statistics(LocalStatistics& statistics,
                                   std::unique_ptr<GlobalJoinStatistics>& global_statistics) {
  std::for_each(statistics.begin(), statistics.end(), [&](auto& stat) { global_statistics->merge(*stat.get()); });
}

}  // namespace statistics

#endif  // SRC_JOIN_STATISTICS_HH
