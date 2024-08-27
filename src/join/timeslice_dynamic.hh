#ifndef TIMESLICE_DYNAMIC_HH
#define TIMESLICE_DYNAMIC_HH

#include "../ontology/uct.hh"
#include "../statistics/join_statistics.hh"
#include "../timing/join_timing.hh"
#include "../util/debug.hh"

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

  double approx_compute_ratio(int64_t last_x, int64_t last_y) {
    double ratio_x = static_cast<double>(last_x - start.x) / static_cast<double>(end.x - start.x);
    double larger_x = std::exp2(std::ceil(std::log2(ratio_x)));
    return std::pow(ratio_x / larger_x, 2);
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
      if ((end.x - start.x) / ALIGNMENT > 1 && (end.y - start.y) / ALIGNMENT > 1) {
        next_block = UL;
      } else if ((end.x - start.x) / ALIGNMENT > 1) {
        next_block = LEFT;
      } else if ((end.y - start.y) / ALIGNMENT > 1) {
        next_block = UPPER;
      } else {
        next_block = DONE;
      }
    }

    Subblock next_block;

    Subblock advance_block() {
      switch (next_block) {
      case UL:
        next_block = UR;
        break;
      case UR:
        next_block = self_join() ? LR : LL;
        break;
      case LL:
        next_block = LR;
        break;
      case LEFT:
        next_block = RIGHT;
        break;
      case UPPER:
        next_block = LOWER;
        break;
      default:
        next_block = DONE;
        break;
      }
      return next_block;
    }

    [[nodiscard]] PlanBlock get_current_subblock() const {
      const int64_t half_x = std::ceil(static_cast<double>(end.x - start.x) / 2 / 64) * 64;
      const int64_t half_y = std::ceil(static_cast<double>(end.y - start.y) / 2 / 64) * 64;

      Corner inner_start, inner_end;

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

      return {inner_start, inner_end};
    }

    [[nodiscard]] bool is_finished() const { return next_block == DONE; }
  };

public:
  explicit BlockScheduler(int64_t total_size) { nested_blocks.emplace_back(0, 0, total_size, total_size); }

  ProcessBlock get_block() {
    auto& innermost_block = nested_blocks.back();
    assert(innermost_block.start.x % ALIGNMENT == 0 && innermost_block.start.y % ALIGNMENT == 0);
    return {innermost_block.start, innermost_block.end};
  }

  ProcessBlock advance_block(int64_t last_x, int64_t last_y) {
    Corner largest_processed_block;
    Corner start = get_block().start;
    if (get_block().end == Corner(last_x, last_y)) {
      largest_processed_block = Corner(last_x, last_y);
      do {
        // block was fully processed
        nested_blocks.pop_back();
        if (!nested_blocks.empty()) {
          auto& lb = nested_blocks.back();
          lb.advance_block();
        }
      } while (nested_blocks.size() > 1 && nested_blocks.back().is_finished());
    } else {
      // block was partially processed
      // find the largest processed subblock
      do {
        auto& lb = nested_blocks.back();
        nested_blocks.emplace_back(lb.get_current_subblock());
      } while (nested_blocks.back().end.x > last_x && nested_blocks.back().end.y > last_y);
      // last block was processed completely
      largest_processed_block = nested_blocks.back().end;
      nested_blocks.pop_back();
      nested_blocks.back().advance_block();
    }
    if (!nested_blocks.empty()) {
      if (nested_blocks.back().is_finished()) {
        nested_blocks.pop_back();
      } else {
        nested_blocks.emplace_back(nested_blocks.back().get_current_subblock());
      }
    }

    return ProcessBlock{start, largest_processed_block};
  }

  Corner next_smallest_fitting(int64_t last_x, int64_t last_y) {
    if (get_block().end == Corner(last_x, last_y)) {
      return {last_x, last_y};
    } else {
      // explicitly taking copy here
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

  [[nodiscard]] bool is_finished() const { return nested_blocks.empty(); }

private:
  std::vector<PlanBlock> nested_blocks;
};

struct VarSizeAlgIns {
  ontology::QueryPlan& plan;
  std::unique_ptr<JoinAlgorithm<MaterializeHandler>> algorithm{};
  ReductionCache& reduction_cache;
  AlgorithmSharedState<>& ass;
  std::vector<types::Dataset> owned_data;
  similarity::Similarity& similarity;
  std::vector<similarity::Similarity> similarities;
  int64_t start{};
  int64_t end{};  // last usable data entry (index might contain later entries, but they should be ignored)

  VarSizeAlgIns(ontology::QueryPlan& plan,
                similarity::Similarity& similarity,
                ReductionCache& reduction_cache,
                AlgorithmSharedState<>& ass,
                int64_t start)
      : plan(plan), reduction_cache(reduction_cache), ass(ass), similarity(similarity), start(start), end(start) {
    similarities = reduction_cache.get_all_reduced_similarities(similarity, plan);
    reset();
  }

  [[nodiscard]] bool initialized() const { return start != end; }
  void reset() {
    owned_data.clear();
    end = start;
    algorithm.reset();
    algorithm = resolve_algorithmid(plan.algorithm_id, plan.steps.empty() ? similarity : similarities.back(), ass);
  }
};

class BlockAlgorithmCache {
public:
  explicit BlockAlgorithmCache(size_t algorithm_count) : algorithms(algorithm_count) {}

public:
  std::optional<util::object_ptr<VarSizeAlgIns>> find_instance(int64_t algorithm_id,
                                                               int64_t range_start,
                                                               int64_t range_end) {
    auto& map = algorithms[algorithm_id];
    util::object_ptr<VarSizeAlgIns> best_fit;
    if (!map.empty()) {
      double best_fitness = 0;

      auto it = map.lower_bound(range_start);
      while (it != map.end()) {
        auto& instance = it->second;
        if (instance.start != range_start) {
          break;
        }
        if (instance.end < range_end) {
          continue;
        }

        double fitness = static_cast<double>(instance.end - range_end) / static_cast<double>(range_end - range_start);
        if (fitness > best_fitness) {
          best_fit = &instance;
          best_fitness = fitness;
        }
      }
    }
    if (best_fit) {
      return std::make_optional(&(*best_fit));
    }

    return std::nullopt;
  }

  void emplace(int64_t algorithm_id, int64_t range_start, VarSizeAlgIns&& instance) {
    algorithms[algorithm_id].emplace(range_start, std::forward<VarSizeAlgIns>(instance));
  }

private:
  std::vector<types::TreeTable<int64_t, VarSizeAlgIns>> algorithms;
};

class DynamicTimeslicing {
public:
  DynamicTimeslicing() : reduction_cache(42) {}

  void execute_join(data::Dataset& dataset,
                    similarity::Similarity& similarity,
                    std::vector<ontology::QueryPlan>& plans,
                    timing::TimeDynamicJoinTiming& timing,
                    std::vector<statistics::LocalDynamicTimeSliceStatistics>& all_statistics) {
    BlockScheduler scheduler(dataset.statistics->count);
    BlockAlgorithmCache algorithm_cache(plans.size());
    ontology::UCT uct = ontology::UCT::from_query_plans(plans);
    AlgorithmSharedState<MaterializeHandler> ass;

    constexpr int64_t HALFBATCH = 64;
    constexpr double TIMESLICE = 0.3;
    double scaled_timeslice = TIMESLICE;

    double total_reward = 0;
    int64_t iterations = 0;
    int64_t non_punctual = 0;
    auto next_weight_update = static_cast<int64_t>(plans.size());

    std::vector<types::ResultPair> result_pairs;
    MaterializeHandler handler(result_pairs);

    timing.join_time.start();
    while (!scheduler.is_finished()) {
      auto selection = uct.select_action();
      auto& plan = plans[selection.action];
      auto& plan_statistics = all_statistics[selection.action];
      plan_statistics.selection_count.inc();

      timing::ExecutionCost start_time = timing::start_cost_measurement();
      timing::ExecutionCost end_time;
      double time_required;
      bool time_exceeded = false;
      int64_t processed_pairs = 0;

      util::print_dbg(absl::StrFormat("Starting new measurement"));

      while (!scheduler.is_finished() && !time_exceeded) {
        auto block = scheduler.get_block();

        util::print_dbg(absl::StrFormat(
          "\tStarting block (%i, %i) -- (%i, %i)", block.start.x, block.start.y, block.end.x, block.end.y));

        int64_t left_id = block.start.x;
        int64_t right_id = block.start.y;
        auto left_alg = algorithm_cache.find_instance(selection.action, block.start.x, block.end.x);
        auto right_alg = algorithm_cache.find_instance(selection.action, block.start.y, block.end.y);

        /*
         * Cases:
         *  1. both algorithms are not initialized: Do incremental probe-and-index in both directions
         *  2. at least one algorithm is initialized and can fully process the block (block.end < alg.end):
         *      Compute the block using the algorithm instance
         */
        if (false) {  // left_alg || right_alg
          util::object_ptr<VarSizeAlgIns> best_alg;
          bool left_is_index = false;
          if (left_alg && right_alg) {
            // select better algorithm (better fit)
            if (left_alg->get()->end < right_alg->get()->end) {
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
          if (left_is_index) {
            int64_t computed_until = compute_with_index(dataset.data,
                                                        similarity,
                                                        *best_alg,
                                                        block.start.y,
                                                        block.end.y,
                                                        time_required,
                                                        scaled_timeslice,
                                                        block.self_join(),
                                                        plan,
                                                        handler,
                                                        plan_statistics);
            // mark square or rectangle as processed
            processed_pairs += (computed_until - block.start.y) * (best_alg->end - best_alg->start);
          } else {
            int64_t computed_until = compute_with_index(dataset.data,
                                                        similarity,
                                                        *best_alg,
                                                        block.start.x,
                                                        block.end.x,
                                                        time_required,
                                                        scaled_timeslice,
                                                        block.self_join(),
                                                        plan,
                                                        handler,
                                                        plan_statistics);
            // mark square or rectangle as processed
            processed_pairs += (computed_until - block.start.y) * (best_alg->end - best_alg->start);
          }
        } else {
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

              perform_twosided_microbatch(dataset.data,
                                          similarity,
                                          block.self_join(),
                                          ibatch,
                                          *new_left_alg,
                                          *new_right_alg,
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

                perform_twosided_microbatch(
                  dataset.data, similarity, false, ibatch, *new_right_alg, *new_left_alg, plan, handler, plan_statistics);
                right_id = real_block_end;
              }
            } else {
              right_id = left_id;
            }

            end_time = timing::end_cost_measurement();
            time_required = timing::get_cost(start_time, end_time);
            if (!time_exceeded && time_required > scaled_timeslice) {
              // break outer loop
              time_exceeded = true;
              // stop processing at next full block border for inner loop
              auto new_end = scheduler.next_smallest_fitting(left_id, right_id);
              left_target_id = new_end.x;
              right_target_id = new_end.y;
              util::print_dbg(absl::StrFormat("\t\tNew target (%i, %i)", left_target_id, right_target_id));
            }
          }
          processed_pairs += computed_pairs(block, left_id, right_id);
          auto largest_block = scheduler.advance_block(left_id, right_id);
          new_left_alg->end = largest_block.end.x;
          if (!block.self_join()) {
            new_right_alg->end = largest_block.end.y;
          }
        }

        util::print_dbg(absl::StrFormat("\tFinished block (%i, %i) -- (%i, %i) until (%i, %i)",
                                        block.start.x,
                                        block.start.y,
                                        block.end.x,
                                        block.end.y,
                                        left_id,
                                        right_id));
      }

      double reward = static_cast<double>(processed_pairs) /
                      static_cast<double>(dataset.statistics->count * (dataset.statistics->count - 1) / 2);
      reward /= (time_required / TIMESLICE);
      util::print_dbg(absl::StrFormat(
        "Reward for action %d: %f (Time: %f, #pairs: %d)", selection.action, reward, time_required, processed_pairs));
      total_reward += reward;
      iterations += 1;

      if (iterations == next_weight_update) {
        double avg_reward = total_reward / static_cast<double>(iterations);
        double next_weight = std::exp2(std::floor(std::log2(avg_reward)));
        uct.update_exp_weight(next_weight);
        next_weight_update *= 2;

        if (static_cast<double>(non_punctual) / static_cast<double>(iterations) > 0.25) {
          scaled_timeslice *= 2;
        }
      }

      uct.update(selection, reward);
    }
    timing.join_time.stop();

    uct.for_each_action(
      [&](ontology::detail::UCTNode& n) { all_statistics[n.get_action()].bandit_weight.record(n.get_mean()); });
  }

private:
  void perform_twosided_microbatch(types::Dataset& data,
                                   similarity::Similarity& similarity,
                                   bool is_self_join,
                                   IndexedBatch& batch,
                                   VarSizeAlgIns& indexing_alg,
                                   VarSizeAlgIns& probing_alg,
                                   ontology::QueryPlan& selected_plan,
                                   MaterializeHandler& handler,
                                   statistics::LocalDynamicTimeSliceStatistics& plan_statistics) {
    // 1. index into indexing_alg
    if (selected_plan.steps.empty()) {
      indexing_alg.algorithm->insert_batch(batch.batch);
    } else {
      std::shared_ptr<types::Dataset> last_level;
      for (int32_t level = static_cast<int32_t>(selected_plan.steps.size()) - 1; level >= 0; --level) {
        last_level =
          reduction_cache.reduce_data_to_level(batch, similarity, selected_plan, level, plan_statistics.rc_statistics);
        int32_t inverted_level = static_cast<int32_t>(selected_plan.steps.size()) - level;
        if (indexing_alg.owned_data.size() < inverted_level) {
          indexing_alg.owned_data.resize(selected_plan.steps.size());
        }
        dataset_append(indexing_alg.owned_data[inverted_level - 1], *last_level);
      }
      auto reduced_batch = dataset_to_batch(*last_level);
      indexing_alg.algorithm->insert_batch(reduced_batch);
    }

    // 2. probe against probing_alg
    std::shared_ptr<std::any> null;
    FilterConfig config{is_self_join ? FilterType::SYMMETRIC_PAIRS : FilterType::NOP};
    if (selected_plan.steps.empty()) {
      probing_alg.algorithm->join_batch(batch.batch, handler, config, plan_statistics, null);
    } else {
      auto reduced =
        reduction_cache.reduce_data_to_end(batch, similarity, selected_plan, plan_statistics.rc_statistics);
      auto reduced_batch = dataset_to_batch(*reduced);

      probing_alg.algorithm->join_batch(reduced_batch, handler, config, plan_statistics, null);
    }

    // 3. verify pairs
    // todo: this should be cleaned up (see algorithm_building_blocks.hh)
    for (int32_t level = 1; level < static_cast<int32_t>(selected_plan.steps.size()); ++level) {
      auto& reduced_index = indexing_alg.owned_data[level];
      auto reduced_index_batch = dataset_to_batch(reduced_index);
      auto reduced_probe =
        reduction_cache.reduce_data_to_level(batch, similarity, selected_plan, level, plan_statistics.rc_statistics);
      auto reduced_probe_batch = dataset_to_batch(*reduced_probe);

      plan_statistics.step_verifications[selected_plan.steps.size() - (level + 1)].add(
        static_cast<int64_t>(handler.results.size()));
      offset_verify_with_similarity(reduced_index_batch,
                                    indexing_alg.start,
                                    reduced_probe_batch,
                                    0,
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

  void perform_onesided_microbatch(types::Dataset& data,
                                   similarity::Similarity& similarity,
                                   IndexedBatch& probing_batch,
                                   VarSizeAlgIns& alg_with_index,
                                   bool is_self_join,
                                   ontology::QueryPlan& selected_plan,
                                   MaterializeHandler& handler,
                                   statistics::LocalDynamicTimeSliceStatistics& plan_statistics) {
    // 1. probe against alg_with_index
    std::shared_ptr<std::any> null;
    FilterConfig config{is_self_join ? CUTOFF_SELFJOIN : CUTOFF, alg_with_index.start, alg_with_index.end};
    if (selected_plan.steps.empty()) {
      alg_with_index.algorithm->join_batch(probing_batch.batch, handler, config, plan_statistics, null);
    } else {
      auto reduced =
        reduction_cache.reduce_data_to_end(probing_batch, similarity, selected_plan, plan_statistics.rc_statistics);
      auto reduced_batch = dataset_to_batch(*reduced);

      alg_with_index.algorithm->join_batch(reduced_batch, handler, config, plan_statistics, null);
    }

    // 2. verify pairs
    plan_statistics.result_size.add(handler.results.size());
    handler.results.clear();
  }

  // returns last probed id
  int64_t compute_with_index(types::Dataset& data,
                             similarity::Similarity& similarity,
                             VarSizeAlgIns& alg_with_index,
                             int64_t probe_block_start,
                             int64_t probe_block_end,
                             double time_required,
                             double scaled_timeslice,
                             bool is_self_join,
                             ontology::QueryPlan& selected_plan,
                             MaterializeHandler& handler,
                             statistics::LocalDynamicTimeSliceStatistics& plan_statistics) {
    int64_t BATCH_SIZE = 64;
    int64_t probe_id = probe_block_start;
    while (probe_id < probe_block_end) {
      auto real_block_end = std::min(probe_id + BATCH_SIZE, probe_block_end);
      auto left_batch = get_batch_by_offset(data, probe_id, real_block_end);
      auto ibatch = IndexedBatch(probe_id, left_batch);

      perform_onesided_microbatch(
        data, similarity, ibatch, alg_with_index, is_self_join, selected_plan, handler, plan_statistics);

      probe_id += real_block_end;
    }

    // todo handle rectangles and timeouts

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
};

// ReSharper restore CppDFANotInitializedField

}  // namespace join::timeslice

#endif  // TIMESLICE_DYNAMIC_HH
