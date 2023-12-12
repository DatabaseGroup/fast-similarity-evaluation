import math
import random

ETA = 0.5


class Algorithm:
    def __init__(self, mean: float, variance: float):
        self.mean = mean
        self.variance = variance
        self.random = random.Random()
        self.weight = 1

    def next(self) -> float:
        return self.random.gauss(self.mean, self.variance)

    def update_weight(self, loss: float):
        self.weight = self.weight * (1 - ETA * loss)

    def reweight(self, total: float):
        self.weight = self.weight / total


def select_from_emp_dist(probabilities: list[float]):
    # assume sum = 1
    cumulative_weight = 0
    selected_weight = random.uniform(0, 1)
    for i, a in enumerate(probabilities):
        if a + cumulative_weight >= selected_weight:
            return i
        cumulative_weight += a


def get_eta(n: int, m: int, r: int) -> float:
    return math.sqrt((2 * (math.log(n) + n * math.log(m))) / (n * (4**r)))


def exp3_light(algorithms: list[Algorithm], trials: int, upper_bound: float):
    alg_count = len(algorithms)
    r = 0
    eta = get_eta(alg_count, trials, r)
    incurred_loss = 0
    total_loss_estimates = [0 for _ in range(alg_count)]

    for i in range(trials):
        probabilities = [
            math.exp(-eta * total_loss_estimates[j] / upper_bound)
            for j in range(alg_count)
        ]
        prob_sum = sum(probabilities)
        probabilities = [j / prob_sum for j in probabilities]
        selected_arm = select_from_emp_dist(probabilities)
        real_loss = algorithms[selected_arm].next()
        incurred_loss += real_loss
        expected_loss = real_loss / probabilities[selected_arm]
        total_loss_estimates[selected_arm] += expected_loss

        min_total_loss = min(total_loss_estimates)
        if min_total_loss / upper_bound > 4**r:
            r = math.ceil(math.log(min_total_loss / upper_bound, 4))
            eta = get_eta(alg_count, trials, r)

    return incurred_loss


LOWER_BOUND = 2
UPPER_BOUND = 50

algorithms = [
    Algorithm(20, 4),
    Algorithm(17, 10),
    Algorithm(25, 5),
    Algorithm(19, 4),
    Algorithm(30, 4),
]  # type: list[Algorithm]


def sim_round(losses: list[list[float]], incurred_losses: list[float]):
    total_weight = sum(map(lambda k: k.weight, algorithms))
    for a in algorithms:
        a.reweight(total_weight)
    total_weight = 1  # after reweighting

    selected_weight = random.uniform(0, total_weight)
    selected_algo = 0
    cumulative_weight = 0
    for i, a in enumerate(algorithms):
        if a.weight + cumulative_weight >= selected_weight:
            selected_algo = i
            break
        cumulative_weight += a.weight

    losses.append(list(map(lambda k: k.next(), algorithms)))
    incurred_losses.append(losses[-1][selected_algo])

    # for alg, loss in zip(algorithms, losses[-1]):
    #    alg.update_weight(loss / UPPER_BOUND)
    algorithms[selected_algo].update_weight(
        (incurred_losses[-1] - 2 * LOWER_BOUND) / UPPER_BOUND
    )

    total_weight = sum(map(lambda k: k.weight, algorithms))
    for a in algorithms:
        a.reweight(total_weight)


if __name__ == "__main__":
    loss = exp3_light(algorithms, 1000, UPPER_BOUND)
    print(f"Experienced loss of {loss}")
