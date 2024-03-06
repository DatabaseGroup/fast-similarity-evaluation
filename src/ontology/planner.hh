#ifndef SRC_PLANNER_HH
#define SRC_PLANNER_HH

#include "../join/join_algorithm.hh"
#include "../join/result_handler.hh"
#include "../similarity/similarity.hh"
#include "../types/types.hh"
#include "reduction.hh"

#include <boost/functional/hash.hpp>

namespace ontology {

class AlgorithmTerminal {
public:
  explicit AlgorithmTerminal(join::AlgorithmId algorithm) : id(algorithm) {}

public:
  const join::AlgorithmId id;
};

class Node;

class ReductionEdge {
public:
  explicit ReductionEdge(Reduction& reduction, Node& to_node) : reduction(reduction), to_node(to_node) {}
  Reduction& reduction;
  Node& to_node;
};

struct NodeKey {
  const types::DatatypeId type;
  const similarity::SimilarityId similarity;

  template <typename H>
  friend H AbslHashValue(H h, const NodeKey& k) {
    return H::combine(std::move(h), k.type, k.similarity);
  }

  bool operator==(const NodeKey& rhs) const { return type == rhs.type && similarity == rhs.similarity; }
  bool operator!=(const NodeKey& rhs) const { return !(rhs == *this); }
};

class Node {
public:
  Node(const types::DatatypeId type, const similarity::SimilarityId similarity) : type(type), similarity(similarity) {}

public:
  const types::DatatypeId type;
  const similarity::SimilarityId similarity;
  std::string comment;
  std::vector<AlgorithmTerminal> algorithms;
  std::vector<ReductionEdge> edges;
};

struct ReductionStep {
  size_t id;
  std::reference_wrapper<Reduction> reduction;
  ReductionStep(size_t id, Reduction& reduction) : id(id), reduction(reduction) {}
};

class QueryPlan {
public:
  std::vector<ReductionStep> steps;
  join::AlgorithmId algorithm_id{join::AlgorithmId::FALLBACK};
};
}  // namespace ontology

// make NodeKey also hashable with std::unordered_map (used for debugging, because absl::flat_hash_map is ugly)
template <>
struct std::hash<ontology::NodeKey> {
  std::size_t operator()(ontology::NodeKey const& n) const noexcept {
    size_t hash = 0;
    boost::hash_combine(hash, n.type);
    boost::hash_combine(hash, n.similarity);
    return hash;
  }
};

namespace ontology {

struct PlannerConfiguration {
  std::vector<join::AlgorithmId> excluded_algorithms;
};

class ReductionGraph {
public:
  virtual ~ReductionGraph() = default;

  std::vector<QueryPlan> enumerate_plans(
    types::DatatypeId type,
    similarity::SimilarityId similarity,
    const PlannerConfiguration& configuration = PlannerConfiguration()) {
    NodeKey node_key{type, similarity};

    // assume it exists
    auto& node = nodes.find(node_key)->second;
    std::vector<QueryPlan> plans;
    QueryPlan empty_plan;

    enumerate_plans_recursive(node, empty_plan, plans, configuration);

    return plans;
  }

private:
  // recursion is fine, the tree does not have high depth (probably)
  // depth depends on the number of types as increasing complexity does not make sense (e.g., represent string as a
  // tree); we only have three types right now and I do not think this will increase to beyond four
  // if this ever becomes an issues, this can easily be rewritten to use iteration (e.g., BFS-like stack management)
  void enumerate_plans_recursive(Node& node,  // NOLINT(*-no-recursion)
                                 QueryPlan& current_plan,
                                 std::vector<QueryPlan>& plans,
                                 const PlannerConfiguration& configuration) {
    // base case
    for (auto& algorithm : node.algorithms) {
      auto& excl_algs = configuration.excluded_algorithms;
      if (std::find(excl_algs.begin(), excl_algs.end(), algorithm.id) == excl_algs.end()) {
        current_plan.algorithm_id = algorithm.id;

        // explicitly make a copy of the plan
        plans.push_back(current_plan);
      }
    }

    // reset current plan
    current_plan.algorithm_id = join::AlgorithmId::FALLBACK;

    // recursive case
    for (auto& edge : node.edges) {
      current_plan.steps.emplace_back(running_reduction_id++, edge.reduction);

      Node& next_node = edge.to_node;
      enumerate_plans_recursive(next_node, current_plan, plans, configuration);

      // remove last reduction step to prepare for next iteration
      current_plan.steps.pop_back();
    }
  }

protected:
  void insert_node(types::DatatypeId type, similarity::SimilarityId similarity) {
    NodeKey key{type, similarity};
    nodes.try_emplace(key, key.type, key.similarity);
  }

  Node& get_node(types::DatatypeId type, similarity::SimilarityId similarity) {
    NodeKey key{type, similarity};
    return nodes.find(key)->second;
  }

protected:
  types::HashTable<NodeKey, Node> nodes;
  std::vector<std::unique_ptr<Reduction>> reductions;

private:
  size_t running_reduction_id{};
};

class StandardReductionGraph : public ReductionGraph {
public:
  StandardReductionGraph() {
    auto& qgram_reduction = *(reductions.emplace_back(std::make_unique<QGramReduction>(3)).get());

    // insert nodes first (otherwise pointers might change)
    insert_node(types::DatatypeId::STRING, similarity::SimilarityId::STRING_EDIT_DISTANCE);
    insert_node(types::DatatypeId::SET, similarity::SimilarityId::QGRAM_COUNT);

    // now get references
    auto& sed = get_node(types::DatatypeId::STRING, similarity::SimilarityId::STRING_EDIT_DISTANCE);
    auto& qgram = get_node(types::DatatypeId::SET, similarity::SimilarityId::QGRAM_COUNT);

    sed.edges.emplace_back(qgram_reduction, qgram);
    sed.algorithms.emplace_back(join::AlgorithmId::PASS_JOIN);
    qgram.algorithms.emplace_back(join::AlgorithmId::PREFIX_SIGNATURE_JOIN);
  }
};

}  // namespace ontology

#endif  // SRC_PLANNER_HH
