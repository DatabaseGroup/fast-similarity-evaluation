#ifndef SRC_PLANNER_HH
#define SRC_PLANNER_HH

#include "../join/join_algorithm.hh"
#include "../similarity/similarity.hh"
#include "../types/types.hh"
#include "reduction.hh"

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

class QueryPlan {
public:
  std::vector<std::reference_wrapper<Reduction>> reduction_steps;
  join::AlgorithmId algorithm_id{join::AlgorithmId::FALLBACK};
};

class ReductionGraph {
public:
  virtual ~ReductionGraph() = default;

  std::vector<QueryPlan> enumerate_plans(types::DatatypeId type, similarity::SimilarityId similarity) {
    NodeKey node_key{type, similarity};

    // assume it exists
    auto& node = nodes.find(node_key)->second;
    std::vector<QueryPlan> plans;
    QueryPlan empty_plan;

    enumerate_plans_recursive(node, empty_plan, plans);

    return plans;
  }

private:
  // recursion is fine, the tree does not have high depth (probably)
  // depth depends on the number of types as increasing complexity does not make sense (e.g., represent string as a
  // tree); we only have three types right now and I do not think this will increase to beyond four
  // if this ever becomes an issues, this can easily be rewritten to use iteration (e.g., BFS-like stack management)
  void enumerate_plans_recursive(Node& node,  // NOLINT(*-no-recursion)
                                 QueryPlan& current_plan,
                                 std::vector<QueryPlan>& plans) {
    // base case
    for (auto& algorithm : node.algorithms) {
      current_plan.algorithm_id = algorithm.id;

      // explicitly make a copy of the plan
      plans.push_back(current_plan);
    }

    // reset current plan
    current_plan.algorithm_id = join::AlgorithmId::FALLBACK;

    // recursive case
    for (auto& edge : node.edges) {
      current_plan.reduction_steps.emplace_back(edge.reduction);

      Node& next_node = edge.to_node;
      enumerate_plans_recursive(next_node, current_plan, plans);

      // remove last reduction step to prepare for next iteration
      current_plan.reduction_steps.pop_back();
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

  types::HashTable<NodeKey, Node> nodes;
  std::vector<std::unique_ptr<Reduction>> reductions;
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
    qgram.algorithms.emplace_back(join::AlgorithmId::PREFIX_SIGNATURE_JOIN);
  }
};

}  // namespace ontology

#endif  // SRC_PLANNER_HH
