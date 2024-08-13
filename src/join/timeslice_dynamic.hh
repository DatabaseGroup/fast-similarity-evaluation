#ifndef TIMESLICE_DYNAMIC_HH
#define TIMESLICE_DYNAMIC_HH
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
  ProcessBlock(const Corner& start, const Corner& end) : Block(start, end) {}

  // creates ""unique"" ids assuming start and end can be stored in 32 bit
  [[nodiscard]] int64_t get_id_for_offset(int64_t x_offset, int64_t y_offset) const {
    assert(end.x <= std::numeric_limits<uint32_t>::max() && end.y <= std::numeric_limits<uint32_t>::max());

    return static_cast<int64_t>(static_cast<uint64_t>(start.x + x_offset) << 32 |
                                static_cast<uint64_t>(start.y + y_offset));
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

  void advance_block(int64_t last_x, int64_t last_y) {

    if (get_block().end == Corner(last_x, last_y)) {
      do {
        // block was fully processed
        nested_blocks.pop_back();
        auto& lb = nested_blocks.back();
        lb.advance_block();
      } while (nested_blocks.size() > 1 && nested_blocks.back().is_finished());
    } else {
      // block was partially processed
      // find largest processed subblock
      do {
        auto& lb = nested_blocks.back();
        nested_blocks.emplace_back(lb.get_current_subblock());
      } while (nested_blocks.back().end.x > last_x && nested_blocks.back().end.y > last_y);
      // last block was processed completely
      nested_blocks.pop_back();
      nested_blocks.back().advance_block();
    }
    if (nested_blocks.back().is_finished()) {
      nested_blocks.pop_back();
    } else {
      nested_blocks.emplace_back(nested_blocks.back().get_current_subblock());
    }
  }

  [[nodiscard]] bool is_finished() const { return nested_blocks.empty(); }

  double approx_compute_ratio(int64_t last_x, int64_t last_y) {
    auto lb = get_block();
    double ratio_x = static_cast<double>(last_x - lb.start.x) / static_cast<double>(lb.end.x - lb.start.x);
    double larger_x = std::exp2(std::ceil(std::log2(ratio_x)));
    return std::pow(ratio_x / larger_x, 2);
  }

private:
  std::vector<PlanBlock> nested_blocks;
};

inline void do_the_thing(data::Dataset& dataset,
                         similarity::Similarity& similarity,
                         std::vector<ontology::QueryPlan>& plans) {
  BlockScheduler scheduler(dataset.statistics->count);

  std::mt19937 prng(std::random_device{}.operator()());
  std::normal_distribution<> dist(10000, 3000);
  int64_t iterations = 0;
  while (!scheduler.is_finished()) {
    auto block = scheduler.get_block();

    auto progress_x = static_cast<int64_t>(std::round(dist(prng)));
    progress_x = std::max(INT64_C(1), std::min(block.end.x - block.start.x, progress_x));

    int64_t progress_y;
    if (std::abs((block.end.x - block.start.x) - progress_x) <= 1) {
      progress_y = block.end.y - block.start.y;
    } else {
      progress_y = progress_x;
    }

    double ratio = scheduler.approx_compute_ratio(block.start.x + progress_x, block.start.y + progress_y);

    util::print_dbg(absl::StrFormat("Iteration %i: processed block (%i, %i)--(%i, %i) until (%i, %i) (Progress = %f)",
                                    ++iterations,
                                    block.start.x,
                                    block.start.y,
                                    block.end.x,
                                    block.end.y,
                                    block.start.x + progress_x,
                                    block.start.y + progress_y,
                                    ratio));

    scheduler.advance_block(block.start.x + progress_x, block.start.y + progress_y);
  }
}
// ReSharper restore CppDFANotInitializedField

}  // namespace join::timeslice

#endif  // TIMESLICE_DYNAMIC_HH
