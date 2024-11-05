#ifndef SRC_UCT_HH
#define SRC_UCT_HH

#include <vector>

#include "../util/object_ptr.hh"

namespace ontology {

namespace detail {

struct UCTConfig {
  double exploration_weight;
};

class UCTNode {
public:
  explicit UCTNode(std::shared_ptr<UCTConfig> config) : config(config) {}

  [[nodiscard]] double action_quality(const UCTNode& child) const {
    double mean_reward = child.total_reward / child.nr_of_selections;
    double bias = config->exploration_weight * std::sqrt(std::log(nr_of_selections) / child.nr_of_selections);

    return mean_reward + bias;
  }

  std::vector<util::object_ptr<UCTNode>> select_path() {
    std::vector<util::object_ptr<UCTNode>> path;
    path.emplace_back(this);
    select_path(path);
    return path;
  }

  void update_path(const std::vector<util::object_ptr<UCTNode>>& path, double reward, double implicit_tries = 1) {
    for (auto node : path) {
      node->total_reward += reward;
      node->nr_of_selections += implicit_tries;
    }
  }

  [[nodiscard]] int64_t get_action() const { return action; }

  void set_action(int64_t action) { this->action = action; }

  util::object_ptr<UCTNode> add_child() {
    untried_action_ids.push_back(actions.size());
    return &actions.emplace_back(config);
  }

  void for_each_action(const std::function<void(UCTNode&)>& fun) {  // NOLINT(*-no-recursion)
    if (actions.empty()) {
      fun(*this);
    } else {
      for (auto& child : actions) {
        child.for_each_action(fun);
      }
    }
  }

  [[nodiscard]] double get_mean() const { return total_reward / nr_of_selections; }

  void reset() {  // NOLINT(*-no-recursion)
    total_reward = 0;
    nr_of_selections = 0;
    untried_action_ids.clear();
    for (size_t i = 0; i < actions.size(); ++i) {
      auto& child = actions[i];
      untried_action_ids.push_back(i);
      child.reset();
    }
  }

private:
  void select_path(std::vector<util::object_ptr<UCTNode>>& path) {  // NOLINT(*-no-recursion)
    if (actions.empty()) {
      return;
    }

    if (!untried_action_ids.empty()) {
      auto id = untried_action_ids.back();
      untried_action_ids.pop_back();
      path.emplace_back(&actions[id]);
    } else {
      // find best action to take
      double best_quality = -1;
      util::object_ptr<UCTNode> best_action = &actions.front();

      for (auto& child : actions) {
        double quality = action_quality(child);

        if (quality > best_quality) {
          best_quality = quality;
          best_action = &child;
        }
      }

      path.push_back(best_action);
    }

    path.back()->select_path(path);
  }

private:
  int64_t action{-1};  // only available if leaf
  double total_reward{};
  double nr_of_selections{};
  std::shared_ptr<UCTConfig> config;

  std::vector<UCTNode> actions;
  std::vector<size_t> untried_action_ids;
};

}  // namespace detail

class UCT {
public:
  using UCTNode = detail::UCTNode;
  using UCTConfig = detail::UCTConfig;

  class Selection {
  public:
    Selection(int64_t action, const std::vector<util::object_ptr<UCTNode>>& path) : action(action), path(path) {}
    int64_t action;
    std::vector<util::object_ptr<UCTNode>> path;
  };

public:
  // arguably hacky
  static UCT from_query_plans(std::vector<QueryPlan>& plans) {
    // maps from reduction step ids to uct nodes
    std::unordered_map<size_t, util::object_ptr<UCTNode>> nodes;

    UCT uct;
    int64_t plan_id = 0;
    for (auto& plan : plans) {
      util::object_ptr parent(&uct.root);
      for (auto& step : plan.steps) {
        auto it = nodes.find(step.id);

        util::object_ptr<UCTNode> node;
        if (it == nodes.end()) {
          node = parent->add_child();
          nodes.emplace(std::pair(step.id, node));
        } else {
          node = it->second;
        }

        parent = node;
      }
      parent->add_child()->set_action(plan_id);
      ++plan_id;
    }

    return uct;
  }

public:
  Selection select_action() {
    auto path = root.select_path();
    auto action = path.back()->get_action();
    return {action, path};
  }

  void update(const Selection& selection, double reward, double implicit_tries = 1.) {
    root.update_path(selection.path, reward, implicit_tries);
  }

  void for_each_action(const std::function<void(UCTNode&)>& fun) { root.for_each_action(fun); }

  void update_exp_weight(double weight) { config->exploration_weight = weight; }

  void reset() { root.reset(); }

private:
  // a reasonable estimated upper bound for avg. reward * sqrt(2)
  std::shared_ptr<UCTConfig> config = std::make_shared<UCTConfig>(std::sqrt(2) * 1e-2);
  UCTNode root{config};
};

}  // namespace ontology

#endif  // SRC_UCT_HH
