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
        if (start.x == start.y) {
          // self join
          missing_blocks = {LR, UR, UL};
        } else {
          missing_blocks = {LR, LL, UR, UL};
        }
      } else if ((end.x - start.x) / ALIGNMENT > 1) {
        missing_blocks = {RIGHT, LEFT};
      } else if ((end.y - start.y) / ALIGNMENT > 1) {
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
  std::unique_ptr<JoinAlgorithm<MaterializeHandler>> algorithm{};
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
      : similarity(similarity), start(start), end(start) {
    similarities = reduction_cache.get_all_reduced_similarities(similarity, plan);
    algorithm = resolve_algorithmid(plan.algorithm_id, plan.steps.empty() ? similarity : similarities.front(), ass);
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

      // todo: Replace this with lower_bound (or similar) again
      auto it = map.begin();
      for (; it != map.end(); ++it) {
        auto& instance = it->second;
        if (instance.start <= range_start && range_end <= instance.end) {
          double fitness =
            static_cast<double>(range_end - range_start) / static_cast<double>(instance.end - instance.start);
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
      absl::StrFormat("\t\tIndexed action %i with range (%i, %i)", algorithm_id, instance.start, instance.end));
    algorithms[algorithm_id].emplace(instance.end, std::forward<VarSizeAlgIns>(instance));
  }

  void print() {
    util::print_dbg("\t\tCurrent index cache: ", "");
    for (size_t action = 0; action < algorithms.size(); ++action) {
      auto& map = algorithms[action];
      for (auto& it : map) {
        auto& alg = it.second;
        util::print_dbg(absl::StrFormat("Algorithm %i with range (%i, %i); ", action, alg.start, alg.end), "");
      }
    }
    util::print_dbg("");
  }

  void print_covered() {
    std::cerr << "\t\tCurrent index cache coverage: \n";
    for (size_t action = 0; action < algorithms.size(); ++action) {
      auto& map = algorithms[action];
      std::cerr << "\t\t\tAlgorithm " << action << ": ";
      int64_t range_start = 0;
      int64_t range_end = 0;
      for (auto& it : map) {
        auto& alg = it.second;
        if (range_start <= alg.start && alg.start <= range_end && range_end <= alg.end) {
          range_end = alg.end;
        }
        if (range_end < alg.start) {
          std::cerr << absl::StrFormat("(%i, %i), ", range_start, range_end);
          range_start = alg.start;
          range_end = alg.end;
        }
      }
      std::cerr << absl::StrFormat("(%i, %i), ", range_start, range_end);
    }
    std::cerr << std::endl;
  }

private:
  std::vector<types::TreeMTable<int64_t, VarSizeAlgIns>> algorithms;
};

template <int64_t MINIMAL_BATCH = 64>
class DynamicTimeslicing {
public:
  explicit DynamicTimeslicing(int64_t dataset_size)
      : reduction_cache(2 * dataset_size / MINIMAL_BATCH), probing_cache(2 * dataset_size / MINIMAL_BATCH) {}

  void execute_join(data::Dataset& dataset,
                    similarity::Similarity& similarity,
                    std::vector<ontology::QueryPlan>& plans,
                    timing::TimeDynamicJoinTiming& timing,
                    std::vector<statistics::LocalDynamicTimeSliceStatistics>& all_statistics) {
    // TJoin does not support the required filter configs and updates
    std::erase_if(plans, [](ontology::QueryPlan& p) { return p.algorithm_id == TJOIN; });

    BlockScheduler<MINIMAL_BATCH> scheduler(dataset.statistics->count);
    BlockAlgorithmCache algorithm_cache(plans.size());
    ontology::UCT uct = ontology::UCT::from_query_plans(plans);
    std::vector<AlgorithmSharedState<>> plan_shared_states(plans.size());

    constexpr int64_t HALFBATCH = MINIMAL_BATCH;
    constexpr double TIMESLICE = 0.0001;
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
      auto& ass = plan_shared_states[selection.action];
      plan_statistics.selection_count.inc();

      timing::ExecutionCost start_time = timing::start_cost_measurement();
      timing::ExecutionCost end_time;
      double time_required;
      bool time_exceeded = false;
      int64_t processed_pairs = 0;

      util::print_dbg(absl::StrFormat("Starting new measurement"));

      while (!scheduler.is_finished() && !time_exceeded) {
        auto block = scheduler.get_block();

        util::print_dbg(absl::StrFormat("\tStarting block with left: (%i, %i), right: (%i, %i)",
                                        block.start.x,
                                        block.end.x,
                                        block.start.y,
                                        block.end.y));

        int64_t left_id = block.start.x;
        int64_t right_id = block.start.y;

        algorithm_cache.print();
        auto left_alg = algorithm_cache.find_instance(selection.action, block.start.x, block.end.x);
        auto right_alg = algorithm_cache.find_instance(selection.action, block.start.y, block.end.y);

        /*
         * Cases:
         *  1. both algorithms are not initialized: Do incremental probe-and-index in both directions
         *  2. at least one algorithm is initialized and can fully process the block
         *     (alg.start <= block.start < block.end <= alg.end): Compute the block using the algorithm instance
         */
        if (left_alg || right_alg) {
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
            absl::StrFormat("\t\tProcessing using cached index for range (%i, %i)", best_alg->start, best_alg->end));

          if (left_is_index) {
            int64_t computed_until = compute_with_index(dataset.data,
                                                        similarity,
                                                        {block.start.x, block.end.x},
                                                        {block.start.y, block.end.y},
                                                        *best_alg,
                                                        block.self_join(),
                                                        selection.action,
                                                        plan,
                                                        handler,
                                                        plan_statistics);
            // mark square or rectangle as processed
            scheduler.advance_block(block.end.x, block.end.y);
            if (block.self_join()) {
              processed_pairs += (computed_until - block.start.y) * (best_alg->end - best_alg->start - 1) / 2;
            } else {
              processed_pairs += (computed_until - block.start.y) * (best_alg->end - best_alg->start);
            }
          } else {
            int64_t computed_until = compute_with_index(dataset.data,
                                                        similarity,
                                                        {block.start.y, block.end.y},
                                                        {block.start.x, block.end.x},
                                                        *best_alg,
                                                        block.self_join(),
                                                        selection.action,
                                                        plan,
                                                        handler,
                                                        plan_statistics);
            // mark square or rectangle as processed
            scheduler.advance_block(block.end.x, block.end.y);
            if (block.self_join()) {
              processed_pairs += (computed_until - block.end.x) * (best_alg->end - best_alg->start - 1) / 2;
            } else {
              processed_pairs += (computed_until - block.end.x) * (best_alg->end - best_alg->start);
            }
          }

          util::print_dbg(
            absl::StrFormat("\tFinished block left: (%i, %i), right (%i, %i) completely using cache index",
                            block.start.x,
                            block.end.x,
                            block.start.y,
                            block.end.y));

          end_time = timing::end_cost_measurement();
          time_required = timing::get_cost(start_time, end_time);
          if (time_required > scaled_timeslice) {
            // break outer loop
            time_exceeded = true;
          }
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
          scheduler.advance_block(left_id, right_id);
          new_left_alg->end = left_target_id;

          algorithm_cache.emplace(selection.action, std::move(*new_left_alg));
          if (!block.self_join()) {
            new_right_alg->end = right_target_id;
            algorithm_cache.emplace(selection.action, std::move(*new_right_alg));
          }

          util::print_dbg(absl::StrFormat("\tFinished block left: (%i, %i), right: (%i, %i) until (%i, %i) using 2sp",
                                          block.start.x,
                                          block.end.x,
                                          block.start.y,
                                          block.end.y,
                                          left_id,
                                          right_id));
        }
      }

      const double all_pairs =
        static_cast<double>(dataset.statistics->count) * (static_cast<double>(dataset.statistics->count) - 1) / 2;
      double reward = static_cast<double>(processed_pairs) / all_pairs;

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
                                   int64_t plan_id,
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
        if (indexing_alg.owned_data.size() < selected_plan.steps.size()) {
          indexing_alg.owned_data.resize(selected_plan.steps.size());
        }
        dataset_append(indexing_alg.owned_data[level], *last_level);
      }
      auto batch_size = std::visit([](auto& b) { return b.data.size(); }, batch.batch);
      auto to_insert_batch = dataset_last_n(indexing_alg.owned_data.front(), batch_size);
      indexing_alg.algorithm->insert_batch(to_insert_batch);
    }

    // 2. probe against probing_alg
    std::shared_ptr<std::any> cached_probing_signatures;
    FilterConfig config{is_self_join ? FilterType::SYMMETRIC_PAIRS : FilterType::NOP};
    if (selected_plan.steps.empty()) {
      if (probing_alg.algorithm->has_independent_probing_signatures()) {
        cached_probing_signatures = probing_cache.get_cached_probing_signatures(
          plan_id, batch.id, batch.batch, *probing_alg.algorithm, plan_statistics.rc_statistics);
      }
      probing_alg.algorithm->join_batch(batch.batch, handler, config, plan_statistics, cached_probing_signatures);
    } else {
      auto reduced =
        reduction_cache.reduce_data_to_end(batch, similarity, selected_plan, plan_statistics.rc_statistics);
      auto reduced_batch = dataset_to_batch(*reduced);
      if (probing_alg.algorithm->has_independent_probing_signatures()) {
        cached_probing_signatures = probing_cache.get_cached_probing_signatures(
          plan_id, batch.id, reduced_batch, *probing_alg.algorithm, plan_statistics.rc_statistics);
      }

      probing_alg.algorithm->join_batch(reduced_batch, handler, config, plan_statistics, cached_probing_signatures);
    }

    // 3. verify pairs
    // todo: this should be cleaned up (see algorithm_building_blocks.hh)
    if (!handler.results.empty()) {
      for (int32_t level = 1; level < static_cast<int32_t>(selected_plan.steps.size()); ++level) {
        auto& reduced_index = probing_alg.owned_data[level];
        auto reduced_index_batch = dataset_to_batch(reduced_index);
        auto reduced_probe =
          reduction_cache.reduce_data_to_level(batch, similarity, selected_plan, level, plan_statistics.rc_statistics);
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
        // todo initialize
        // plan_statistics.step_verifications.back().add(static_cast<int64_t>(handler.results.size()));
        verify_with_similarity(data, similarity, handler.results);
      }
      types::print_result_pairs(std::cerr, handler.results, data);
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
    if (index_range.first <= 6 && 6 <= index_range.second) {
      std::cerr << "..." << std::endl;
    }

    // 1. probe against alg_with_index
    std::shared_ptr<std::any> cached_probing_signatures;
    FilterConfig config{is_self_join ? CUTOFF_SELFJOIN : CUTOFF, index_range.first, index_range.second};
    if (selected_plan.steps.empty()) {
      if (alg_with_index.algorithm->has_independent_probing_signatures()) {
        cached_probing_signatures = probing_cache.get_cached_probing_signatures(
          plan_id, probing_batch.id, probing_batch.batch, *alg_with_index.algorithm, plan_statistics.rc_statistics);
      }
      alg_with_index.algorithm->join_batch(
        probing_batch.batch, handler, config, plan_statistics, cached_probing_signatures);
    } else {
      auto reduced =
        reduction_cache.reduce_data_to_end(probing_batch, similarity, selected_plan, plan_statistics.rc_statistics);
      auto reduced_batch = dataset_to_batch(*reduced);

      if (alg_with_index.algorithm->has_independent_probing_signatures()) {
        cached_probing_signatures = probing_cache.get_cached_probing_signatures(
          plan_id, probing_batch.id, reduced_batch, *alg_with_index.algorithm, plan_statistics.rc_statistics);
      }

      alg_with_index.algorithm->join_batch(reduced_batch, handler, config, plan_statistics, cached_probing_signatures);
    }

    // 2. verify pairs
    // todo: this should be cleaned up (see algorithm_building_blocks.hh)
    for (int32_t level = 1; level < static_cast<int32_t>(selected_plan.steps.size()); ++level) {
      auto& reduced_index = alg_with_index.owned_data[level];
      auto reduced_index_batch = dataset_to_batch(reduced_index);
      auto reduced_probe = reduction_cache.reduce_data_to_level(
        probing_batch, similarity, selected_plan, level, plan_statistics.rc_statistics);
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
      // todo initialize
      // plan_statistics.step_verifications.back().add(static_cast<int64_t>(handler.results.size()));
      verify_with_similarity(data, similarity, handler.results);
    }
    plan_statistics.result_size.add(handler.results.size());
    types::print_result_pairs(std::cerr, handler.results, data);
    handler.results.clear();
  }

  // returns last probed id
  int64_t compute_with_index(types::Dataset& data,
                             similarity::Similarity& similarity,
                             std::pair<int64_t, int64_t> index_range,
                             std::pair<int64_t, int64_t> probe_range,
                             VarSizeAlgIns& alg_with_index,
                             bool is_self_join,
                             int64_t plan_id,
                             ontology::QueryPlan& selected_plan,
                             MaterializeHandler& handler,
                             statistics::LocalDynamicTimeSliceStatistics& plan_statistics) {
    constexpr int64_t BATCH_SIZE = MINIMAL_BATCH;
    int64_t probe_id = probe_range.first;
    while (probe_id < probe_range.second) {
      auto real_block_end = std::min(probe_id + BATCH_SIZE, probe_range.second);
      auto probe_batch = get_batch_by_offset(data, probe_id, real_block_end);
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
};

// ReSharper restore CppDFANotInitializedField

}  // namespace join::timeslice

#endif  // TIMESLICE_DYNAMIC_HH
