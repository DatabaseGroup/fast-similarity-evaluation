#ifndef TIMESLICE_DYNAMIC_HH
#define TIMESLICE_DYNAMIC_HH

#include "../ontology/uct.hh"
#include "../statistics/join_statistics.hh"
#include "../timing/join_timing.hh"
#include "../util/debug.hh"
#include "../util/unstable_erase.hh"

namespace join::timeslice {

struct Corner {
  int64_t x{0};
  int64_t y{0};

  Corner() = default;
  Corner(int64_t x, int64_t y) : x(x), y(y) {}

  friend bool operator==(const Corner& lhs, const Corner& rhs) { return lhs.x == rhs.x && lhs.y == rhs.y; }
};

struct Block {
  // ReSharper disable CppDFANotInitializedField
  Corner start;
  Corner end;
  // ReSharper restore CppDFANotInitializedField

  Block(int64_t start_x, int64_t start_y, int64_t end_x, int64_t end_y) : start(start_x, start_y), end(end_x, end_y) {}
  Block(const Corner& start, const Corner& end) : start(start), end(end) {}

  [[nodiscard]] bool self_join() const { return start.x == start.y; }
};

struct ProcessBlock : Block {
  using Id = int64_t;

  ProcessBlock(const Corner& start, const Corner& end) : Block(start, end) {}

  [[nodiscard]] Id get_id() const { return get_id_for_offset(0, 0); }

  // creates ""unique"" ids assuming start and end can be stored in 32 bit
  [[nodiscard]] Id get_id_for_offset(int64_t x_offset, int64_t y_offset) const {
    assert(end.x <= std::numeric_limits<uint32_t>::max() && end.y <= std::numeric_limits<uint32_t>::max());

    return static_cast<int64_t>(static_cast<uint64_t>(start.x + x_offset) << 32 |
                                static_cast<uint64_t>(start.y + y_offset));
  }
};

// ReSharper disable CppDFANotInitializedField
template <int64_t ALIGNMENT = 64>
class BlockScheduler {
private:
  struct PlanBlock : Block {
    enum Subblock { UL, UR, LL, LR, LEFT, RIGHT, UPPER, LOWER, DONE };
    PlanBlock(int64_t start_x, int64_t start_y, int64_t end_x, int64_t end_y)
        : PlanBlock(Corner(start_x, start_y), Corner(end_x, end_y)) {}
    PlanBlock(const Corner& start, const Corner& end) : Block(start, end) {
      if (static_cast<double>(end.x - start.x) / ALIGNMENT > 1. &&
          static_cast<double>(end.y - start.y) / ALIGNMENT > 1.) {
        if (start.x == start.y) {
          // self join
          missing_blocks = {LR, UR, UL};
        } else {
          missing_blocks = {LR, LL, UR, UL};
        }
      } else if (static_cast<double>(end.x - start.x) / ALIGNMENT > 1.) {
        missing_blocks = {RIGHT, LEFT};
      } else if (static_cast<double>(end.y - start.y) / ALIGNMENT > 1.) {
        missing_blocks = {LOWER, UPPER};
      }
    }
    std::vector<Subblock> missing_blocks;

    Subblock advance_block() {
      if (missing_blocks.empty()) {
        return DONE;
      } else {
        missing_blocks.pop_back();
        return missing_blocks.back();
      }
    }

    [[nodiscard]] PlanBlock get_current_subblock() const {
      const int64_t half_x = std::ceil(static_cast<double>(end.x - start.x) / 2 / ALIGNMENT) * ALIGNMENT;
      const int64_t half_y = std::ceil(static_cast<double>(end.y - start.y) / 2 / ALIGNMENT) * ALIGNMENT;

      Corner inner_start, inner_end;

      if (!missing_blocks.empty()) {
        auto next_block = missing_blocks.back();
        switch (next_block) {
        case UL:
        case UR:
        case UPPER:
          inner_start.y = start.y;
          inner_end.y = start.y + half_y;
          break;
        case LL:
        case LR:
        case LOWER:
          inner_start.y = start.y + half_y;
          inner_end.y = end.y;
          break;
        case LEFT:
        case RIGHT:
          inner_start.y = start.y;
          inner_end.y = end.y;
        default:;
        }

        switch (next_block) {
        case UL:
        case LL:
        case LEFT:
          inner_start.x = start.x;
          inner_end.x = start.x + half_x;
          break;
        case UR:
        case LR:
        case RIGHT:
          inner_start.x = start.x + half_x;
          inner_end.x = end.x;
          break;
        case UPPER:
        case LOWER:
          inner_start.x = start.x;
          inner_end.x = end.x;
        default:;
        }
      }

      return {inner_start, inner_end};
    }

    [[nodiscard]] bool is_finished() const { return missing_blocks.empty(); }
  };

public:
  explicit BlockScheduler(int64_t total_size) { nested_blocks.emplace_back(0, 0, total_size, total_size); }

  ProcessBlock get_block() {
    auto& innermost_block = nested_blocks.back();
    assert(innermost_block.start.x % ALIGNMENT == 0 && innermost_block.start.y % ALIGNMENT == 0);
    return {innermost_block.start, innermost_block.end};
  }

  void advance_block(int64_t last_x, int64_t last_y) {
    /*
     *  Three cases:
     *    1. block was completely processed
     *    2. block was completely processed in one direction
     *    3. block was not completely processed in both directions
     */
    const auto& last_block = get_block();
    if (last_block.end == Corner(last_x, last_y)) {
      do {
        // block was fully processed
        nested_blocks.pop_back();
        if (!nested_blocks.empty()) {
          auto& lb = nested_blocks.back();
          lb.advance_block();
        }
      } while (nested_blocks.size() > 1 && nested_blocks.back().is_finished());

      if (!nested_blocks.empty()) {
        if (nested_blocks.back().is_finished()) {
          nested_blocks.pop_back();
        } else {
          nested_blocks.emplace_back(nested_blocks.back().get_current_subblock());
        }
      }
    } else if (last_block.end.x == last_x || last_block.end.y == last_y) {
      auto last_start = last_block.start;
      auto last_end = last_block.end;
      nested_blocks.pop_back();
      if (last_block.end.x == last_x) {
        nested_blocks.emplace_back(Corner(last_start.x, last_y), last_end);
      } else {
        nested_blocks.emplace_back(Corner(last_x, last_start.y), last_end);
      }
    } else {
      // block was partially processed
      // find the largest processed subblock
      do {
        auto& lb = nested_blocks.back();
        nested_blocks.emplace_back(lb.get_current_subblock());
      } while (nested_blocks.back().end.x > last_x && nested_blocks.back().end.y > last_y);
      // last block was processed completely
      nested_blocks.pop_back();
      nested_blocks.back().advance_block();

      if (!nested_blocks.empty()) {
        if (nested_blocks.back().is_finished()) {
          nested_blocks.pop_back();
        } else {
          nested_blocks.emplace_back(nested_blocks.back().get_current_subblock());
        }
      }
    }
  }

  Corner next_larger_fitting(int64_t last_x, int64_t last_y) {
    if (get_block().end == Corner(last_x, last_y)) {
      return {last_x, last_y};
    } else {
      const auto& block = get_block();
      PlanBlock lb_outer{block.start, block.end};
      PlanBlock lb_inner = lb_outer.get_current_subblock();
      while (last_x < lb_inner.end.x || last_y < lb_inner.end.y) {
        lb_outer = lb_inner;
        lb_inner = lb_outer.get_current_subblock();
      }
      return lb_outer.end;
    }
  }

  Corner next_smaller_fitting(int64_t last_x, int64_t last_y) {
    if (get_block().end == Corner(last_x, last_y)) {
      return {last_x, last_y};
    } else {
      const auto& block = get_block();
      PlanBlock lb_inner{block.start, block.end};
      lb_inner = lb_inner.get_current_subblock();
      while (last_x < lb_inner.end.x || last_y < lb_inner.end.y) {
        lb_inner = lb_inner.get_current_subblock();
      }
      return lb_inner.end;
    }
  }

  [[nodiscard]] bool is_finished() const { return nested_blocks.empty(); }

private:
  std::vector<PlanBlock> nested_blocks;
};

struct VarSizeAlgIns {
  std::unique_ptr<JoinAlgorithm<MaterializeHandler>> algorithm{};
  std::vector<types::Dataset> owned_data;
  similarity::Similarity& similarity;
  std::vector<similarity::Similarity> similarities;
  int64_t start{};
  int64_t end{};  // last usable data entry (index might contain later entries, but they should be ignored)
  bool deleted = false;

  VarSizeAlgIns(ontology::QueryPlan& plan,
                similarity::Similarity& similarity,
                ReductionCache& reduction_cache,
                AlgorithmSharedState<>& ass,
                int64_t start)
      : similarity(similarity), start(start), end(start) {
    similarities = reduction_cache.get_all_reduced_similarities(similarity, plan);
    algorithm = resolve_algorithmid(plan.algorithm_id, plan.steps.empty() ? similarity : similarities.front(), ass);
  }

  VarSizeAlgIns(VarSizeAlgIns&& other) noexcept = default;
  VarSizeAlgIns& operator=(VarSizeAlgIns&& other) noexcept {
    algorithm = std::move(other.algorithm);
    owned_data = std::move(other.owned_data);
    similarity = std::move(other.similarity);
    similarities = std::move(other.similarities);
    start = other.start;
    end = other.end;
    return *this;
  };

  [[nodiscard]] int64_t size() const { return end - start; }
};

class BlockAlgorithmCache {
public:
  explicit BlockAlgorithmCache(size_t algorithm_count) : algorithms(algorithm_count) {}

public:
  std::optional<util::object_ptr<VarSizeAlgIns>> find_covered_instance(int64_t algorithm_id,
                                                                       int64_t range_start,
                                                                       int64_t range_end) {
    return find_instance_by_predicate(
      algorithm_id,
      range_start,
      range_end,
      [](int64_t range_start, int64_t range_end, int64_t instance_start, int64_t instance_end) {
        return instance_start <= range_start && range_end <= instance_end;
      });
  }

  std::optional<util::object_ptr<VarSizeAlgIns>> find_starting_instance(
    int64_t algorithm_id,
    const std::pair<int64_t, int64_t>& index_range) {
    return find_instance_by_predicate(
      algorithm_id,
      index_range.first,
      index_range.second,
      [&](int64_t range_start,
          [[maybe_unused]] int64_t range_end,
          int64_t instance_start,
          [[maybe_unused]] int64_t instance_end) { return instance_start == range_start; });
  }

  std::optional<util::object_ptr<VarSizeAlgIns>> find_instance_by_predicate(
    int64_t algorithm_id,
    int64_t range_start,
    int64_t range_end,
    const std::function<bool(int64_t, int64_t, int64_t, int64_t)>& predicate) {
    auto& map = algorithms[algorithm_id];
    util::object_ptr<VarSizeAlgIns> best_fit;
    if (!map.empty()) {
      double best_fitness = 0;

      auto it = map.begin();
      for (; it != map.end(); ++it) {
        auto& instance = *it;
        if (predicate(range_start, range_end, instance.start, instance.end)) {
          int64_t overlap = std::min(range_end, instance.end) - std::max(range_start, instance.start);
          int64_t total_range = range_end - range_start + instance.end - instance.start;
          double fitness = static_cast<double>(overlap) / static_cast<double>(total_range);
          if (fitness > best_fitness) {
            best_fit = &instance;
            best_fitness = fitness;
          }
        }
      }
    }
    if (best_fit) {
      return std::make_optional(&*best_fit);
    }

    return std::nullopt;
  }

  void emplace(int64_t algorithm_id, VarSizeAlgIns&& instance) {
    util::print_dbg(
      absl::StrFormat("\t\tIndexed action %i with range (%i, %i)", algorithm_id, instance.start, instance.end),
      util::DEBUG);
    algorithms[algorithm_id].emplace_back(std::forward<VarSizeAlgIns>(instance));
  }

  void print() const {
    util::print_dbg("\t\tCurrent index cache: ", util::INFO, "");
    for (size_t action = 0; action < algorithms.size(); ++action) {
      auto& map = algorithms[action];
      for (auto& it : map) {
        auto& alg = it;
        util::print_dbg(
          absl::StrFormat("Algorithm %i with range (%i, %i); ", action, alg.start, alg.end), util::INFO, "");
      }
    }
    util::print_dbg("", util::INFO);
  }

  [[nodiscard]] int64_t get_coverage(size_t algorithm_id) const {
    auto& map = algorithms[algorithm_id];
    int64_t coverage = 0;
    for (auto& it : map) {
      coverage += it.end - it.start;
    }
    return coverage;
  }

  void consolidate() {
    int32_t deduplicated_indexes = 0;
    int32_t merged_indexes = 0;
    print();

    for (auto& alg : algorithms) {
      if (alg.size() >= 2) {
        std::ranges::sort(alg, [](auto& i1, auto& i2) {
          if (i1.start == i2.start) {
            return i2.end < i1.end;
          } else {
            return i1.start < i2.start;
          }
        });

        // remove algorithms completely included in another algorithm
        std::vector<VarSizeAlgIns> dedup_algs;
        {
          types::TreeSet<int64_t> range_ends;
          for (auto& a : alg) {
            auto it = range_ends.lower_bound(a.end);
            if (it == range_ends.end()) {
              dedup_algs.emplace_back(std::move(a));
            } else {
              ++deduplicated_indexes;
            }
            range_ends.insert(a.end);
          }
        }

        // get mean algorithm instance size, only merge up to mean size
        int64_t total_size = 0;
        std::vector<int64_t> sizes;
        std::ranges::for_each(alg, [&](auto& i) {
          total_size += i.size();
          sizes.push_back(i.size());
        });

        int64_t mean = total_size / static_cast<int64_t>(alg.size());
        std::ranges::sort(sizes);
        int64_t median = sizes[sizes.size() / 2];
        int64_t merge_threshold = std::max(mean, median);
        // blocks have unequal sizes due to rounding, so give this some amount of fuzz
        merge_threshold += merge_threshold / 10;

        util::print_dbg(absl::StrFormat("Merge Threshold: %f", merge_threshold));

        // merge neighboring algorithms as long as they are small enough
        bool merge_occured;

        std::vector<VarSizeAlgIns> merged_algs = std::move(dedup_algs);
        do {
          merge_occured = false;
          types::HashTable<int64_t, std::vector<size_t>> start_to_offset;

          std::vector<VarSizeAlgIns> new_algs;

          for (size_t i = 0; i < merged_algs.size(); ++i) {
            auto& a = merged_algs[i];
            start_to_offset[a.start].push_back(i);
          }

          for (auto& a : merged_algs) {
            if (a.deleted) {
              continue;
            }

            bool found_mergeable = false;
            std::vector<size_t>::iterator found_offset;

            auto it = start_to_offset.find(a.end);
            if (it != start_to_offset.end()) {
              auto& list = it->second;

              for (auto off_it = list.begin(); off_it != list.end(); ++off_it) {
                auto& b = merged_algs[*off_it];

                if (a.algorithm->supports_merge() && b.algorithm->supports_merge() &&
                    static_cast<int64_t>(a.size() + b.size()) <= merge_threshold) {
                  found_mergeable = true;
                  found_offset = off_it;
                  break;
                }
              }
            }

            if (found_mergeable) {
              auto& b = merged_algs[*found_offset];

              // remove from list
              util::unstable_erase(start_to_offset[a.end], found_offset);

              for (size_t i = 0; i < a.owned_data.size(); ++i) {
                auto& o1 = a.owned_data[i];
                auto& o2 = b.owned_data[i];
                types::dataset_append(o1, o2);
              }
              a.algorithm->merge(*b.algorithm);

              a.end = b.end;
              b.deleted = true;

              new_algs.emplace_back(std::move(a));
              merge_occured = true;
              ++merged_indexes;
            } else {
              new_algs.emplace_back(std::move(a));
            }
          }

          merged_algs = std::move(new_algs);
        } while (merge_occured);

        alg = std::move(merged_algs);
      }
    }

    util::print_dbg(absl::StrFormat("Deduped %i and merged %i indexes", deduplicated_indexes, merged_indexes));
    print();
  }

private:
  std::vector<std::vector<VarSizeAlgIns>> algorithms;
};

template <int64_t MINIMAL_BATCH = 32>
class DynamicTimeslicing {
public:
  explicit DynamicTimeslicing(std::vector<ontology::QueryPlan>& plans, int64_t dataset_size, double timeslice)
      : reduction_cache(2 * (dataset_size / MINIMAL_BATCH + 1)),
        probing_cache(2 * (dataset_size / MINIMAL_BATCH + 1)),
        timeslice(timeslice),
        plan_shared_states(plans.size()),
        algorithm_cache(plans.size()) {}

  void execute_join(data::Dataset& dataset,
                    similarity::Similarity& similarity,
                    std::vector<ontology::QueryPlan>& plans,
                    timing::TimeDynamicJoinTiming& timing,
                    std::vector<statistics::LocalDynamicTimeSliceStatistics>& all_statistics) {
    for (size_t action = 0; action < plans.size(); ++action) {
      util::print_dbg(absl::StrFormat("Action %i: %s", action, plans[action].to_string()));
    }

    BlockScheduler<MINIMAL_BATCH> scheduler(dataset.statistics->count);
    // reset UCT
    uct = ontology::UCT::from_query_plans(plans);

    constexpr int64_t HALFBATCH = MINIMAL_BATCH;
    double scaled_timeslice = timeslice;
    // account for higher cost of 2 x probe + 2 x indexing
    double indexing_timeslice = INDEXING_BONUS * scaled_timeslice;

    double total_unweighted_reward = 0;
    double max_reward = 0;
    int64_t iterations = 0;
    int64_t non_punctual = 0;
    int64_t non_punctual_window_size = 0;
    double non_punctual_scale = 1;
    auto next_weight_update = 3 * static_cast<int64_t>(plans.size());
    constexpr int64_t WEIGHT_UPDATE_STEP = 10;

    std::vector<types::ResultPair> result_pairs;
    MaterializeHandler handler(result_pairs);

    timing.join_time.start();
    while (!scheduler.is_finished()) {
      auto selection = uct.select_action();
      auto& plan = plans[selection.action];
      auto& plan_statistics = all_statistics[selection.action];
      auto& ass = plan_shared_states[selection.action];
      plan_statistics.selection_count.inc();

      timing::ExecutionCost start_time = timing::start_cost_measurement();
      timing::ExecutionCost end_time;
      double time_required = 0;
      bool time_exceeded = false;
      bool has_started_using_cache = false;
      int64_t processed_pairs = 0;

      util::print_dbg(absl::StrFormat("Starting new measurement"), util::DEBUG);

      while (!scheduler.is_finished() && !time_exceeded) {
        auto block = scheduler.get_block();
        util::print_dbg(absl::StrFormat("\tStarting block with left: (%i, %i), right: (%i, %i)",
                                        block.start.x,
                                        block.end.x,
                                        block.start.y,
                                        block.end.y),
                        util::DEBUG);

        int64_t left_id = block.start.x;
        int64_t right_id = block.start.y;

        auto left_alg = algorithm_cache.find_covered_instance(selection.action, block.start.x, block.end.x);
        auto right_alg = algorithm_cache.find_covered_instance(selection.action, block.start.y, block.end.y);

        if (!left_alg && !right_alg) {
          left_alg = algorithm_cache.find_starting_instance(selection.action, {block.start.x, block.end.x});
          right_alg = algorithm_cache.find_starting_instance(selection.action, {block.start.y, block.end.y});
        }

        /*
         * Cases:
         *  1. both algorithms are not initialized: Do incremental probe-and-index in both directions
         *  2. at least one algorithm is initialized and can fully process the block
         *     (alg.start <= block.start < block.end <= alg.end): Compute the block using the algorithm instance
         */
        if (left_alg || right_alg) {
          has_started_using_cache = true;
          plan_statistics.index_cache_hits.inc();
          util::object_ptr<VarSizeAlgIns> best_alg;
          bool left_is_index = false;
          if (left_alg && right_alg) {
            // select better algorithm (better fit)
            auto left_range = left_alg->get()->end - left_alg->get()->start;
            auto right_range = right_alg->get()->end - right_alg->get()->start;
            if (left_range < right_range) {
              best_alg = *left_alg;
              left_is_index = true;
            } else {
              best_alg = *right_alg;
            }
          } else if (left_alg) {
            best_alg = *left_alg;
            left_is_index = true;
          } else {
            best_alg = *right_alg;
          }

          util::print_dbg(
            absl::StrFormat("\t\tProcessing using cached index for range (%i, %i)", best_alg->start, best_alg->end),
            util::DEBUG);

          Corner computed_until{};

          if (left_is_index) {
            std::pair<int64_t, int64_t> index_range{block.start.x, block.end.x};
            std::pair<int64_t, int64_t> probe_range{block.start.y, block.end.y};
            if (!(best_alg->start <= block.start.x && block.end.x <= best_alg->end)) {
              assert(best_alg->start == block.start.x);
              auto next_full_block =
                scheduler.next_smaller_fitting(best_alg->end, probe_range.first + (best_alg->end - best_alg->start));
              index_range.second = next_full_block.x;
              probe_range.second = next_full_block.y;
            }
            compute_with_index(dataset.data,
                               similarity,
                               index_range,
                               probe_range,
                               *best_alg,
                               block.self_join(),
                               selection.action,
                               plan,
                               handler,
                               plan_statistics);
            computed_until.x = index_range.second;
            computed_until.y = probe_range.second;
          } else {
            std::pair<int64_t, int64_t> index_range{block.start.y, block.end.y};
            std::pair<int64_t, int64_t> probe_range{block.start.x, block.end.x};
            if (!(best_alg->start <= block.start.y && block.end.y <= best_alg->end)) {
              assert(best_alg->start == block.start.y);
              auto next_full_block =
                scheduler.next_smaller_fitting(probe_range.first + (best_alg->end - best_alg->start), best_alg->end);
              index_range.second = next_full_block.y;
              probe_range.second = next_full_block.x;
            }
            compute_with_index(dataset.data,
                               similarity,
                               index_range,
                               probe_range,
                               *best_alg,
                               block.self_join(),
                               selection.action,
                               plan,
                               handler,
                               plan_statistics);
            computed_until.x = probe_range.second;
            computed_until.y = index_range.second;
          }

          scheduler.advance_block(computed_until.x, computed_until.y);
          int64_t new_pairs;
          if (block.self_join()) {
            new_pairs = (computed_until.x - block.start.x) * (computed_until.y - block.start.y - 1) / 2;
          } else {
            new_pairs = (computed_until.x - block.start.x) * (computed_until.y - block.start.y);
          }
          util::print_dbg(absl::StrFormat("\t\tComputed %i pairs.", new_pairs), util::DEBUG);
          processed_pairs += new_pairs;

          util::print_dbg(
            absl::StrFormat("\tFinished block left: (%i, %i), right (%i, %i) %s using cache index until (%i, %i)",
                            block.start.x,
                            block.end.x,
                            block.start.y,
                            block.end.y,
                            computed_until.x == block.end.x && computed_until.y == block.end.y   ? "completely"
                            : computed_until.x == block.end.x || computed_until.y == block.end.y ? "semi-completely"
                                                                                                 : "incompletely",
                            computed_until.x,
                            computed_until.y),
            util::DEBUG);

          end_time = timing::end_cost_measurement();
          time_required = timing::get_cost(start_time, end_time);
          if (time_required > scaled_timeslice) {
            // break outer loop
            time_exceeded = true;
          }
        } else {
          if (has_started_using_cache) {
            util::print_dbg("Stopping to reduce fragmentation", util::DEBUG);
            time_exceeded = true;
          } else {
            plan_statistics.index_cache_misses.inc();
            // case 1: probe-and-insert
            int64_t left_target_id = block.end.x;
            int64_t right_target_id = block.end.y;
            // initialize algorithm
            auto new_left_alg = std::make_shared<VarSizeAlgIns>(plan, similarity, reduction_cache, ass, left_id);
            std::shared_ptr<VarSizeAlgIns> new_right_alg;
            if (block.self_join()) {
              new_right_alg = new_left_alg;
            } else {
              new_right_alg = std::make_shared<VarSizeAlgIns>(plan, similarity, reduction_cache, ass, right_id);
            }

            while (left_id < left_target_id || right_id < right_target_id) {
              if (left_id < left_target_id) {
                auto real_block_end = std::min(left_id + HALFBATCH, left_target_id);
                auto left_batch = get_batch_by_offset(dataset.data, left_id, real_block_end);
                auto ibatch = IndexedBatch(left_id, left_batch);

                new_left_alg->end = real_block_end;
                perform_twosided_microbatch(dataset.data,
                                            similarity,
                                            block.self_join(),
                                            ibatch,
                                            *new_left_alg,
                                            *new_right_alg,
                                            selection.action,
                                            plan,
                                            handler,
                                            plan_statistics);
                left_id = real_block_end;
              }

              if (!block.self_join()) {
                if (right_id < right_target_id) {
                  auto real_block_end = std::min(right_id + HALFBATCH, right_target_id);
                  auto right_batch = get_batch_by_offset(dataset.data, right_id, real_block_end);
                  auto ibatch = IndexedBatch(right_id, right_batch);

                  new_right_alg->end = real_block_end;
                  perform_twosided_microbatch(dataset.data,
                                              similarity,
                                              false,
                                              ibatch,
                                              *new_right_alg,
                                              *new_left_alg,
                                              selection.action,
                                              plan,
                                              handler,
                                              plan_statistics);
                  right_id = real_block_end;
                }
              } else {
                right_id = left_id;
              }

              end_time = timing::end_cost_measurement();
              time_required = timing::get_cost(start_time, end_time);
              if (!time_exceeded && time_required > indexing_timeslice) {
                // break outer loop
                time_exceeded = true;
                // stop processing at next full block border for inner loop
                auto new_end = scheduler.next_larger_fitting(left_id, right_id);
                left_target_id = new_end.x;
                right_target_id = new_end.y;
                util::print_dbg(absl::StrFormat("\t\tNew target (%i, %i)", left_target_id, right_target_id),
                                util::DEBUG);
              }
            }
            auto new_pairs = computed_pairs(block, left_id, right_id);
            util::print_dbg(absl::StrFormat("\t\tComputed %i pairs.", new_pairs), util::DEBUG);
            processed_pairs += new_pairs;
            scheduler.advance_block(left_id, right_id);

            algorithm_cache.emplace(selection.action, std::move(*new_left_alg));
            if (!block.self_join()) {
              algorithm_cache.emplace(selection.action, std::move(*new_right_alg));
            }

            util::print_dbg(absl::StrFormat("\tFinished block left: (%i, %i), right: (%i, %i) until (%i, %i) using 2sp",
                                            block.start.x,
                                            block.end.x,
                                            block.start.y,
                                            block.end.y,
                                            left_id,
                                            right_id),
                            util::DEBUG);
          }
        }
      }

      end_time = timing::end_cost_measurement();
      time_required = timing::get_cost(start_time, end_time);
      if (time_required / indexing_timeslice > 1.25) {
        non_punctual++;
      }

      const double all_pairs =
        static_cast<double>(dataset.statistics->count) * (static_cast<double>(dataset.statistics->count) - 1) / 2;
      double reward = static_cast<double>(processed_pairs) / all_pairs;
      total_unweighted_reward += reward;
      double no_of_ts = time_required / timeslice;
      if (!has_started_using_cache) {
        no_of_ts /= INDEXING_BONUS;
      }
      reward = reward / no_of_ts;

      util::print_dbg(absl::StrFormat("Reward for action %d: %f (Time: %f, Tries: %f, #pairs: %d)",
                                      selection.action,
                                      reward,
                                      time_required,
                                      no_of_ts,
                                      processed_pairs));
      max_reward = std::max(reward, max_reward);
      iterations += 1;
      non_punctual_window_size += 1;

      if (iterations >= next_weight_update) {
        double next_weight = std::max(0., 1. * (1 - 1.5 * total_unweighted_reward)) * max_reward;
        util::print_dbg(absl::StrFormat("Updating UCT weights to %f", next_weight));
        uct.update_exp_weight(next_weight);
        next_weight_update += WEIGHT_UPDATE_STEP;
        if (static_cast<double>(non_punctual) / static_cast<double>(non_punctual_window_size) > 0.25) {
          non_punctual = 0;
          non_punctual_window_size = 0;
          non_punctual_scale *= 2;
        }
        scaled_timeslice = timeslice * non_punctual_scale * (1 + std::min(2., 4 * total_unweighted_reward));
        indexing_timeslice = INDEXING_BONUS * scaled_timeslice;
        util::print_dbg(absl::StrFormat(
          "Increasing timeslice to %f, indexing timeslice to %f", scaled_timeslice, indexing_timeslice));
      }

      uct.update(selection, reward, no_of_ts);
    }
    timing.join_time.stop();

    for (size_t i = 0; i < plans.size(); ++i) {
      auto& statistic = all_statistics[i];
      statistic.indexed_ratio =
        static_cast<double>(algorithm_cache.get_coverage(i)) / static_cast<double>(dataset.statistics->count);
    }
    algorithm_cache.consolidate();
    uct.for_each_action(
      [&](const ontology::detail::UCTNode& n) { all_statistics[n.get_action()].bandit_weight.record(n.get_mean()); });
  }

private:
  void perform_twosided_microbatch(types::Dataset& data,
                                   similarity::Similarity& similarity,
                                   bool is_self_join,
                                   IndexedBatch& batch,
                                   VarSizeAlgIns& indexing_alg,
                                   VarSizeAlgIns& probing_alg,
                                   int64_t plan_id,
                                   ontology::QueryPlan& selected_plan,
                                   MaterializeHandler& handler,
                                   statistics::LocalDynamicTimeSliceStatistics& plan_statistics) {
    // 1. index into indexing_alg
    if (selected_plan.steps.empty()) {
      auto indexed_data = dataset_to_batch(data, indexing_alg.start, indexing_alg.end);
      indexing_alg.algorithm->insert_batch(indexed_data, batch.batch);
    } else {
      std::shared_ptr<types::Dataset> last_level;
      for (int32_t level = static_cast<int32_t>(selected_plan.steps.size()) - 1; level >= 0; --level) {
        last_level = reduction_cache.reduce_data_to_level(batch, selected_plan, level, plan_statistics.rc_statistics);
        if (indexing_alg.owned_data.size() < selected_plan.steps.size()) {
          indexing_alg.owned_data.resize(selected_plan.steps.size());
        }
        dataset_append(indexing_alg.owned_data[level], *last_level);
      }
      auto batch_size = std::visit([](auto& b) { return b.data.size(); }, batch.batch);
      auto indexed_data = dataset_to_batch(indexing_alg.owned_data.front());
      auto to_insert_batch = dataset_last_n(indexing_alg.owned_data.front(), batch_size);
      indexing_alg.algorithm->insert_batch(indexed_data, to_insert_batch);
    }

    // 2. probe against probing_alg
    // only probe if the instance contains any data
    if (probing_alg.start != probing_alg.end) {
      std::shared_ptr<std::any> cached_probing_signatures;
      FilterConfig config{is_self_join ? FilterType::SYMMETRIC_PAIRS : FilterType::NOP};
      if (selected_plan.steps.empty()) {
        if (probing_alg.algorithm->has_independent_probing_signatures()) {
          cached_probing_signatures = probing_cache.get_cached_probing_signatures(
            plan_id, batch.id, batch.batch, *probing_alg.algorithm, plan_statistics.rc_statistics);
        }
        auto indexed_data = dataset_to_batch(data, probing_alg.start, probing_alg.end);
        probing_alg.algorithm->join_batch(
          indexed_data, batch.batch, handler, config, plan_statistics, cached_probing_signatures);
      } else {
        // it might be the case that no data was inserted into the probing_alg instance yet (this is the case for every
        // first iteration); we have to skip this in that case (there is no indexed_data to be used)

        auto reduced = reduction_cache.reduce_data_to_end(batch, selected_plan, plan_statistics.rc_statistics);
        auto reduced_batch = dataset_to_batch(*reduced);
        if (probing_alg.algorithm->has_independent_probing_signatures()) {
          cached_probing_signatures = probing_cache.get_cached_probing_signatures(
            plan_id, batch.id, reduced_batch, *probing_alg.algorithm, plan_statistics.rc_statistics);
        }
        auto indexed_data = dataset_to_batch(probing_alg.owned_data.front());

        probing_alg.algorithm->join_batch(
          indexed_data, reduced_batch, handler, config, plan_statistics, cached_probing_signatures);
      }
    }

    // 3. verify pairs
    // todo: this should be cleaned up (see algorithm_building_blocks.hh)
    if (!handler.results.empty()) {
      for (int32_t level = 1; level < static_cast<int32_t>(selected_plan.steps.size()); ++level) {
        auto& reduced_index = probing_alg.owned_data[level];
        auto reduced_index_batch = dataset_to_batch(reduced_index);
        auto reduced_probe =
          reduction_cache.reduce_data_to_level(batch, selected_plan, level, plan_statistics.rc_statistics);
        auto reduced_probe_batch = dataset_to_batch(*reduced_probe);

        plan_statistics.step_verifications[selected_plan.steps.size() - (level + 1)].add(
          static_cast<int64_t>(handler.results.size()));
        offset_verify_with_similarity(reduced_index_batch,
                                      probing_alg.start,
                                      reduced_probe_batch,
                                      static_cast<int64_t>(batch.id),  // = first id
                                      indexing_alg.similarities[level],
                                      handler.results);
      }

      // if data was actually reduced, we still have to verify with the "outermost" similarity
      // otherwise, the algorithm instance has already verified this part
      if (!selected_plan.steps.empty()) {
        plan_statistics.step_verifications.back().add(static_cast<int64_t>(handler.results.size()));
        verify_with_similarity(data, similarity, handler.results);
      }
      // types::print_result_pairs(std::cerr, handler.results, data);
      plan_statistics.result_size.add(handler.results.size());
      handler.results.clear();
    }
  }

  void perform_onesided_microbatch(types::Dataset& data,
                                   similarity::Similarity& similarity,
                                   IndexedBatch& probing_batch,
                                   std::pair<int64_t, int64_t> index_range,
                                   VarSizeAlgIns& alg_with_index,
                                   bool is_self_join,
                                   int64_t plan_id,
                                   ontology::QueryPlan& selected_plan,
                                   MaterializeHandler& handler,
                                   statistics::LocalDynamicTimeSliceStatistics& plan_statistics) {
    // 1. probe against alg_with_index
    std::shared_ptr<std::any> cached_probing_signatures;
    FilterConfig config{is_self_join ? CUTOFF_SELFJOIN : CUTOFF, index_range.first, index_range.second};
    if (selected_plan.steps.empty()) {
      if (alg_with_index.algorithm->has_independent_probing_signatures()) {
        cached_probing_signatures = probing_cache.get_cached_probing_signatures(
          plan_id, probing_batch.id, probing_batch.batch, *alg_with_index.algorithm, plan_statistics.rc_statistics);
      }

      auto indexed_data = dataset_to_batch(data, alg_with_index.start, alg_with_index.end);

      alg_with_index.algorithm->join_batch(
        indexed_data, probing_batch.batch, handler, config, plan_statistics, cached_probing_signatures);
    } else {
      auto reduced = reduction_cache.reduce_data_to_end(probing_batch, selected_plan, plan_statistics.rc_statistics);
      auto reduced_batch = dataset_to_batch(*reduced);

      if (alg_with_index.algorithm->has_independent_probing_signatures()) {
        cached_probing_signatures = probing_cache.get_cached_probing_signatures(
          plan_id, probing_batch.id, reduced_batch, *alg_with_index.algorithm, plan_statistics.rc_statistics);
      }

      auto indexed_data = dataset_to_batch(alg_with_index.owned_data.front());
      alg_with_index.algorithm->join_batch(
        indexed_data, reduced_batch, handler, config, plan_statistics, cached_probing_signatures);
    }

    // 2. verify pairs
    // todo: this should be cleaned up (see algorithm_building_blocks.hh)
    for (int32_t level = 1; level < static_cast<int32_t>(selected_plan.steps.size()); ++level) {
      auto& reduced_index = alg_with_index.owned_data[level];
      auto reduced_index_batch = dataset_to_batch(reduced_index);
      auto reduced_probe =
        reduction_cache.reduce_data_to_level(probing_batch, selected_plan, level, plan_statistics.rc_statistics);
      auto reduced_probe_batch = dataset_to_batch(*reduced_probe);

      plan_statistics.step_verifications[selected_plan.steps.size() - (level + 1)].add(
        static_cast<int64_t>(handler.results.size()));
      offset_verify_with_similarity(reduced_index_batch,
                                    alg_with_index.start,
                                    reduced_probe_batch,
                                    static_cast<int64_t>(probing_batch.id),
                                    alg_with_index.similarities[level],
                                    handler.results);
    }

    // if data was actually reduced, we still have to verify with the "outermost" similarity
    // otherwise, the algorithm instance has already verified this part
    if (!selected_plan.steps.empty()) {
      plan_statistics.step_verifications.back().add(static_cast<int64_t>(handler.results.size()));
      verify_with_similarity(data, similarity, handler.results);
    }
    plan_statistics.result_size.add(handler.results.size());
    // types::print_result_pairs(std::cerr, handler.results, data);
    handler.results.clear();
  }

  // returns last probed id
  int64_t compute_with_index(types::Dataset& data,
                             similarity::Similarity& similarity,
                             const std::pair<int64_t, int64_t>& index_range,
                             const std::pair<int64_t, int64_t>& probe_range,
                             VarSizeAlgIns& alg_with_index,
                             bool is_self_join,
                             int64_t plan_id,
                             ontology::QueryPlan& selected_plan,
                             MaterializeHandler& handler,
                             statistics::LocalDynamicTimeSliceStatistics& plan_statistics) {
    constexpr int64_t BATCH_SIZE = MINIMAL_BATCH;
    int64_t probe_id = probe_range.first;
    while (probe_id < probe_range.second) {
      const auto real_block_end = std::min(probe_id + BATCH_SIZE, probe_range.second);
      const auto probe_batch = get_batch_by_offset(data, probe_id, real_block_end);
      auto ibatch = IndexedBatch(probe_id, probe_batch);

      perform_onesided_microbatch(data,
                                  similarity,
                                  ibatch,
                                  index_range,
                                  alg_with_index,
                                  is_self_join,
                                  plan_id,
                                  selected_plan,
                                  handler,
                                  plan_statistics);

      probe_id = real_block_end;
    }

    return probe_id;
  }

  int64_t computed_pairs(ProcessBlock& block, int64_t end_x, int64_t end_y) {
    int64_t triangle =
      ((end_x - block.start.x) * (end_y - block.start.y) - std::min(end_x - block.start.x, end_y - block.start.y)) / 2;
    if (!block.self_join()) {
      triangle *= 2;
    }
    return triangle;
  }

private:
  ReductionCache reduction_cache;
  ProbingSignaturesCache probing_cache;
  const double timeslice;
  std::vector<AlgorithmSharedState<>> plan_shared_states;
  BlockAlgorithmCache algorithm_cache;
  ontology::UCT uct;
  const double INDEXING_BONUS = 4.;
};

// ReSharper restore CppDFANotInitializedField

}  // namespace join::timeslice

#endif  // TIMESLICE_DYNAMIC_HH
