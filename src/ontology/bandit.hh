#ifndef SRC_BANDIT_HH
#define SRC_BANDIT_HH

#include <absl/random/random.h>

namespace ontology {

class Exp3Light {
public:
  Exp3Light(int64_t arms, int64_t trials, double lossBound) : arms(arms), trials(trials), loss_bound(lossBound) {
    weight.resize(arms, 1);
    weight_sum = static_cast<double>(arms);
    expected_total_loss.resize(arms, 0);

    current_trial = 0;
    epoch = 0;

    min_total_loss = std::numeric_limits<double>::max();
    update_eta();
  }

public:
  int64_t select_arm() {
    double rand = absl::Uniform(gen, 0, weight_sum);
    double rolling_sum = 0;

    for (int64_t arm = 0; arm < arms; ++arm) {
      rolling_sum += weight[arm];

      if (rand <= rolling_sum) {
        return arm;
      }
    }

    // should not happen
    return arms - 1;
  }

  void update_weights(int64_t selected_arm, double loss) {
    // unbiased estimator for loss (loss of selection / probability of selection)
    double loss_estimate = loss / (weight[selected_arm] / weight_sum);

    expected_total_loss[selected_arm] += loss_estimate;
    // update weight
    double previous_weight = weight[selected_arm];
    double new_weight = std::exp(-eta * expected_total_loss[selected_arm] / loss_bound);
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

  void restart_with_loss_bound(double new_lb) {
    loss_bound = new_lb;
    std::fill(weight.begin(), weight.end(), 1);
    weight_sum = static_cast<double>(arms);
    std::fill(expected_total_loss.begin(), expected_total_loss.end(), 0);
    min_total_loss = std::numeric_limits<double>::max();

    epoch = 0;
    trials = trials - current_trial;
    current_trial = 0;

    min_total_loss = std::numeric_limits<double>::max();
    update_eta();
  }

private:
  void update_eta() {
    eta = std::sqrt((2 * (std::log(arms) + static_cast<double>(arms) * std::log(trials))) /
                    (static_cast<double>(arms) * std::pow(4, epoch)));
  }

private:
  int64_t arms;
  int64_t trials;
  int64_t current_trial;
  double loss_bound;
  int64_t epoch;
  double eta;
  std::vector<double> weight;
  double weight_sum;
  std::vector<double> expected_total_loss;
  double min_total_loss;
  absl::BitGen gen;
};

class Exp3LightA {
public:
  Exp3LightA(int64_t arms, int64_t trials) : arms(arms), trials(trials), epoch(0), loss_bound(1), bandit_solver(arms, trials, loss_bound), current_trial(1)  {

  }

  int64_t select_arm() {
    return bandit_solver.select_arm();
  }

  void update_weights(int64_t selected_arm, double loss) {
    if (loss > loss_bound) {
      int64_t u = std::ceil(std::log2(loss));
      loss_bound = std::pow(2, u);
      bandit_solver.restart_with_loss_bound(loss_bound);
    } else {
      bandit_solver.update_weights(selected_arm, loss);
    }
  }

private:
  int64_t arms;
  int64_t trials;
  int64_t current_trial;

  int64_t epoch;
  double loss_bound;
  Exp3Light bandit_solver;
};

}  // namespace ontology

#endif  // SRC_BANDIT_HH
