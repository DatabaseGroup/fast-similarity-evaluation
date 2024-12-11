#ifndef SRC_PLANNER_HH
#define SRC_PLANNER_HH

#include <boost/functional/hash.hpp>

#include "../join/join_algorithm.hh"
#include "../join/result_handler.hh"
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

struct ReductionStep {
  size_t id;
  std::reference_wrapper<Reduction> reduction;
  ReductionStep(size_t id, Reduction& reduction) : id(id), reduction(reduction) {}

  friend bool operator==(const ReductionStep& lhs, const ReductionStep& rhs) { return lhs.id == rhs.id; }
  friend bool operator!=(const ReductionStep& lhs, const ReductionStep& rhs) { return !(lhs == rhs); }

  friend std::size_t hash_value(const ReductionStep& obj) {
    std::size_t seed = 0x482AFEFB;
    boost::hash_combine(seed, obj.id);
    return seed;
  }
};

class QueryPlan {
public:
  std::vector<ReductionStep> steps;
  join::AlgorithmId algorithm_id{join::AlgorithmId::FALLBACK};

  [[nodiscard]] std::string to_string() const {
    std::string res;

    for (auto& step : steps) {
      res += step.reduction.get().get_label() + "->";
    }
    res += join::algorithm_to_string(algorithm_id);

    return res;
  }

  friend bool operator==(const QueryPlan& lhs, const QueryPlan& rhs) {
    return lhs.steps == rhs.steps && lhs.algorithm_id == rhs.algorithm_id;
  }
  friend bool operator!=(const QueryPlan& lhs, const QueryPlan& rhs) { return !(lhs == rhs); }

  friend std::size_t hash_value(const QueryPlan& obj) {
    std::size_t seed = 0x6565EE34;
    boost::hash_combine(seed, obj.steps);
    boost::hash_combine(seed, obj.algorithm_id);
    return seed;
  }
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
  std::vector<std::string> excluded_reductions;
};

class ReductionGraph {
public:
  virtual ~ReductionGraph() = default;

  std::vector<QueryPlan> enumerate_plans(types::DatatypeId type,
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
      auto& excl_red = configuration.excluded_reductions;
      if (std::find(excl_red.begin(), excl_red.end(), edge.reduction.get_label()) == excl_red.end()) {
        current_plan.steps.emplace_back(running_reduction_id++, edge.reduction);

        Node& next_node = edge.to_node;
        enumerate_plans_recursive(next_node, current_plan, plans, configuration);

        // remove last reduction step to prepare for next iteration
        current_plan.steps.pop_back();
      }
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
  explicit StandardReductionGraph(const std::vector<std::string>& additional_reductions) {
    auto& traversal_string_reduction = *reductions.emplace_back(std::make_unique<TraversalStringReduction>());
    auto& q3gram_reduction = *reductions.emplace_back(std::make_unique<QGramReduction>(3));
    auto& label_set_reduction = *reductions.emplace_back(std::make_unique<LabelSetReduction>());
    auto& jaro_set_reduction = *reductions.emplace_back(std::make_unique<JaroSetReduction>(1));

    size_t red_start = reductions.size();
    for (auto& s : additional_reductions) {
      if (!s.empty() && s[0] == 'q') {
        int64_t q = std::stoi(s.substr(1));
        reductions.emplace_back(std::make_unique<QGramReduction>(q));
      }
    }
    std::vector<std::unique_ptr<Reduction>*> additional_qram_reductions;
    additional_qram_reductions.reserve(reductions.size());
    for (auto i = red_start; i < reductions.size(); ++i) {
      additional_qram_reductions.emplace_back(&reductions[i]);
    }

    // insert nodes first (otherwise pointers might change)
    insert_node(types::DatatypeId::TREE, similarity::SimilarityId::TREE_EDIT_DISTANCE);
    insert_node(types::DatatypeId::STRING, similarity::SimilarityId::STRING_EDIT_DISTANCE);
    insert_node(types::DatatypeId::STRING, similarity::SimilarityId::JARO_STRING);
    insert_node(types::DatatypeId::SET, similarity::SimilarityId::STRUCTUAL_SET_SIM);
    insert_node(types::DatatypeId::SET, similarity::SimilarityId::HAMMING_DISTANCE);
    insert_node(types::DatatypeId::SET, similarity::SimilarityId::JACCARD);
    insert_node(types::DatatypeId::SET, similarity::SimilarityId::JARO_OVERLAP);

    // now get references
    auto& ted = get_node(types::DatatypeId::TREE, similarity::SimilarityId::TREE_EDIT_DISTANCE);
    auto& sed = get_node(types::DatatypeId::STRING, similarity::SimilarityId::STRING_EDIT_DISTANCE);
    auto& jaro_string = get_node(types::DatatypeId::STRING, similarity::SimilarityId::JARO_STRING);
    auto& struct_set_sim = get_node(types::DatatypeId::SET, similarity::SimilarityId::STRUCTUAL_SET_SIM);
    auto& set_hd = get_node(types::DatatypeId::SET, similarity::SimilarityId::HAMMING_DISTANCE);
    auto& jaccard = get_node(types::DatatypeId::SET, similarity::SimilarityId::JACCARD);
    auto& jaro_overlap = get_node(types::DatatypeId::SET, similarity::SimilarityId::JARO_OVERLAP);

    ted.edges.emplace_back(traversal_string_reduction, sed);
    ted.edges.emplace_back(label_set_reduction, struct_set_sim);
    sed.edges.emplace_back(q3gram_reduction, struct_set_sim);
    for (auto qgram_reduction : additional_qram_reductions) {
      sed.edges.emplace_back(**qgram_reduction, struct_set_sim);
    }

    jaro_string.edges.emplace_back(jaro_set_reduction, jaro_overlap);
    sed.algorithms.emplace_back(join::AlgorithmId::PASS_JOIN);
    struct_set_sim.algorithms.emplace_back(join::AlgorithmId::PREFIX_SIGNATURE_JOIN);
    struct_set_sim.algorithms.emplace_back(join::AlgorithmId::PALLOC);
    struct_set_sim.algorithms.emplace_back(join::AlgorithmId::PARTITION);
    set_hd.algorithms.emplace_back(join::AlgorithmId::PREFIX_SIGNATURE_JOIN);
    ted.algorithms.emplace_back(join::AlgorithmId::TJOIN);
    jaccard.algorithms.emplace_back(join::AlgorithmId::PREFIX_SIGNATURE_JOIN);
    jaccard.algorithms.emplace_back(join::AlgorithmId::PALLOC);
    jaccard.algorithms.emplace_back(join::AlgorithmId::PARTITION);
    jaro_overlap.algorithms.emplace_back(join::AlgorithmId::PREFIX_SIGNATURE_JOIN);
    jaro_overlap.algorithms.emplace_back(join::AlgorithmId::PALLOC);
    jaro_overlap.algorithms.emplace_back(join::AlgorithmId::PARTITION);
  }
};

}  // namespace ontology

#endif  // SRC_PLANNER_HH
