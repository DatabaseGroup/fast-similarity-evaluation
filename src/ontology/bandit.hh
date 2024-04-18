#ifndef SRC_BANDIT_HH
#define SRC_BANDIT_HH

#include <absl/random/random.h>

namespace ontology {

class Exp3Light {
public:
  Exp3Light(int64_t arms, int64_t trials, long double lossBound)
      : arms(arms), trials(trials), loss_bound(lossBound), eta(0), incurred_loss(0) {
    weight.resize(arms, 1);
    weight_sum = static_cast<long double>(arms);
    expected_total_loss.resize(arms, 0);

    current_trial = 0;
    epoch = 0;

    min_total_loss = std::numeric_limits<long double>::max();
    update_eta();
  }

public:
  int64_t select_arm() {
    long double rand = absl::Uniform(gen, 0, weight_sum);
    long double rolling_sum = 0;

    for (int64_t arm = 0; arm < arms; ++arm) {
      rolling_sum += weight[arm];

      if (rand <= rolling_sum) {
        return arm;
      }
    }

    // should not happen
    return arms - 1;
  }

  void update_weights(int64_t selected_arm, long double loss) {
    incurred_loss += loss;

    // unbiased estimator for loss (loss of selection / probability of selection)
    long double loss_estimate = loss / (weight[selected_arm] / weight_sum);

    expected_total_loss[selected_arm] += loss_estimate;
    // update weight
    long double previous_weight = weight[selected_arm];
    long double new_weight = std::exp(-eta * expected_total_loss[selected_arm] / loss_bound);
    weight[selected_arm] = new_weight;
    weight_sum += new_weight - previous_weight;

    // update min total loss and update
    // minimum value maintenance can be replaced with a min heap if required
    min_total_loss = *std::min_element(expected_total_loss.begin(), expected_total_loss.end());
    if ((min_total_loss / loss_bound) > std::pow(4, epoch)) {
      // epoch = ceil(log_4(min_total_loss / loss_bound))
      epoch = std::ceil(std::log(min_total_loss / loss_bound) / std::log(4));
      update_eta();
    }
  }

  void restart_with_loss_bound(long double new_lb) {
    loss_bound = new_lb;
    std::fill(weight.begin(), weight.end(), 1);
    weight_sum = static_cast<long double>(arms);
    std::fill(expected_total_loss.begin(), expected_total_loss.end(), 0);
    min_total_loss = std::numeric_limits<long double>::max();

    incurred_loss = 0;
    epoch = 0;
    trials = trials - current_trial;
    current_trial = 0;

    min_total_loss = std::numeric_limits<long double>::max();
    update_eta();
  }

  long double get_normalized_weight(int64_t arm) { return weight[arm] / weight_sum; }

  [[nodiscard]] long double get_incurred_loss() const { return incurred_loss; }

  [[nodiscard]] long double get_expected_total_loss(int64_t arm) const { return expected_total_loss[arm]; }

private:
  void update_eta() {
    eta = std::sqrt((2 * (std::log(arms) + static_cast<long double>(arms) * std::log(trials))) /
                    (static_cast<long double>(arms) * std::pow(4, epoch)));
    // weights have to be recalculated due to new eta
    weight_sum = 0;
    for (int64_t arm = 0; arm < arms; ++arm) {
      weight[arm] = std::exp(-eta * expected_total_loss[arm] / loss_bound);
      weight_sum += weight[arm];
    }
  }

private:
  int64_t arms;
  int64_t trials;
  int64_t current_trial;
  long double loss_bound;
  int64_t epoch;
  long double eta;
  std::vector<long double> weight;
  long double weight_sum;
  std::vector<long double> expected_total_loss;
  long double min_total_loss;
  long double incurred_loss;
  absl::BitGen gen;
};

class Exp3LightA {
public:
  Exp3LightA(int64_t arms, int64_t trials) : loss_bound(1), bandit_solver(arms, trials, loss_bound) {}

  int64_t select_arm() { return bandit_solver.select_arm(); }

  void update_weights(int64_t selected_arm, long double loss) {
    if (loss > loss_bound) {
      int64_t u = std::ceil(std::log2(loss));
      loss_bound = std::pow(2, u);
      bandit_solver.restart_with_loss_bound(loss_bound);
    } else {
      bandit_solver.update_weights(selected_arm, loss);
    }
  }

  long double get_normalized_weight(int64_t arm) { return bandit_solver.get_normalized_weight(arm); }

  [[nodiscard]] long double get_incurred_loss() const { return bandit_solver.get_incurred_loss(); }

  [[nodiscard]] long double get_expected_total_loss(int64_t arm) const {
    return bandit_solver.get_expected_total_loss(arm);
  }

private:
  long double loss_bound;
  Exp3Light bandit_solver;
};

}  // namespace ontology

#endif  // SRC_BANDIT_HH
