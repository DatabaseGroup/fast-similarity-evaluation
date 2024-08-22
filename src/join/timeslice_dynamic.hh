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
class BlockScheduler {
private:
  struct PlanBlock : Block {
    enum Subblock { UL, UR, LL, LR, DONE };
    PlanBlock(int64_t start_x, int64_t start_y, int64_t end_x, int64_t end_y)
        : Block(start_x, start_y, end_x, end_y), next_block(UL) {}
    PlanBlock(const Corner& start, const Corner& end) : Block(start, end), next_block(UL) {}

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
      default:
        next_block = DONE;
        break;
      }
      return next_block;
    }

    [[nodiscard]] PlanBlock get_current_subblock() const {
      const int64_t half_x = (end.x - start.x) / 2;
      const int64_t half_y = (end.y - start.y) / 2;

      Corner inner_start, inner_end;

      switch (next_block) {
      case UL:
      case UR:
        inner_start.y = start.y;
        inner_end.y = start.y + half_y;
        break;
      case LL:
      case LR:
        inner_start.y = start.y + half_y;
        inner_end.y = end.y;
        break;
      default:;
      }

      switch (next_block) {
      case UL:
      case LL:
        inner_start.x = start.x;
        inner_end.x = start.x + half_x;
        break;
      case UR:
      case LR:
        inner_start.x = start.x + half_x;
        inner_end.x = end.x;
        break;
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
      // find largest processed subblock
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

class DynamicTimeslicing {
public:
  DynamicTimeslicing() : algorithm_cache(42), reduction_cache(42) {}

  void execute_join(data::Dataset& dataset,
                    similarity::Similarity& similarity,
                    std::vector<ontology::QueryPlan>& plans,
                    timing::TimeDynamicJoinTiming& timing,
                    std::vector<statistics::LocalDynamicTimeSliceStatistics>& all_statistics) {
    BlockScheduler scheduler(dataset.statistics->count);
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

      while (!scheduler.is_finished() && !time_exceeded) {
        auto block = scheduler.get_block();

        int64_t left_id = block.start.x;
        int64_t right_id = block.start.y;
        auto left_alg =
          algorithm_cache.emplace({selection.action, left_id},
                                  std::make_unique<VarSizeAlgIns>(plan, similarity, reduction_cache, ass, left_id));
        auto right_alg =
          algorithm_cache.emplace({selection.action, right_id},
                                  std::make_unique<VarSizeAlgIns>(plan, similarity, reduction_cache, ass, right_id));

        /*
         * TODO: Already out of date, update
         * Cases:
         *  1. both algorithms are not initialized: Do incremental probe-and-index in both directions
         *  2. at least one algorithm is initialized and can fully process the block (block.end < alg.end):
         *      Compute the block using the algorithm instance
         *  3. at least one algorithm is initialized, but both cannot fully process the block:
         *      Reset the algorithm instances, go to case 1
         *
         *  We check the cases in the order 3, 2, 1
         */
        if (left_alg->initialized() || right_alg->initialized()) {
          left_alg->reset();
          right_alg->reset();
        }
        // case 1: probe-and-insert
        // todo: just always process until the end of the next block once timeout is reached (make next end of block the
        // next target)
        while ((!time_exceeded || block.approx_compute_ratio(left_id, right_id) >= 0.5) &&
               (left_id < block.end.x || right_id < block.end.y)) {
          if (left_id < block.end.x) {
            auto real_block_end = std::min(left_id + HALFBATCH, block.end.x);
            auto left_batch = get_batch_by_offset(dataset.data, left_id, real_block_end);
            auto ibatch = IndexedBatch(left_id, left_batch);

            perform_twosided_microbatch(dataset.data,
                                        similarity,
                                        block.self_join(),
                                        ibatch,
                                        *left_alg,
                                        *right_alg,
                                        plan,
                                        handler,
                                        plan_statistics);
            left_id = real_block_end;
          }

          if (!block.self_join() && right_id < block.end.y) {
            auto real_block_end = std::min(right_id + HALFBATCH, block.end.y);
            auto right_batch = get_batch_by_offset(dataset.data, right_id, real_block_end);
            auto ibatch = IndexedBatch(right_id, right_batch);

            perform_twosided_microbatch(
              dataset.data, similarity, false, ibatch, *right_alg, *left_alg, plan, handler, plan_statistics);
            right_id = real_block_end;
          } else {
            right_id = left_id;
          }

          end_time = timing::end_cost_measurement();
          time_required = timing::get_cost(start_time, end_time);
          if (time_required > scaled_timeslice) {
            time_exceeded = true;
          }
        }

        processed_pairs += computed_pairs(block, left_id, right_id);

        auto largest_block = scheduler.advance_block(left_id, right_id);
        left_alg->end = largest_block.end.x;
        if (!block.self_join()) {
          right_alg->end = largest_block.end.y;
        }

        util::print_dbg(absl::StrFormat("Current block (%i, %i) -- (%i, %i) processed until (%i ,%i)",
                                        block.start.x,
                                        block.start.y,
                                        block.end.x,
                                        block.end.y,
                                        left_id,
                                        right_id));
      }

      double reward = static_cast<double>(processed_pairs) /
                      static_cast<double>(dataset.statistics->count * (dataset.statistics->count - 1) / 2);
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
                                   int64_t index_block_end,
                                   ontology::QueryPlan& selected_plan,
                                   MaterializeHandler& handler,
                                   statistics::LocalDynamicTimeSliceStatistics& plan_statistics) {
    // 1. probe against alg_with_index
    std::shared_ptr<std::any> null;
    FilterConfig config{FilterType::CUTOFF, alg_with_index.start, index_block_end};
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

  int64_t computed_pairs(ProcessBlock& block, int64_t end_x, int64_t end_y) {
    int64_t triangle =
      ((end_x - block.start.x) * (end_y - block.start.y) - std::min(end_x - block.start.x, end_y - block.start.y)) / 2;
    if (!block.self_join()) {
      triangle *= 2;
    }
    return triangle;
  }

private:
  util::LRUCache<std::pair<int64_t, ProcessBlock::Id>, std::shared_ptr<VarSizeAlgIns>> algorithm_cache;
  ReductionCache reduction_cache;
};

// ReSharper restore CppDFANotInitializedField

}  // namespace join::timeslice

#endif  // TIMESLICE_DYNAMIC_HH
