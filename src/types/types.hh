#ifndef SRC_TYPES_HH
#define SRC_TYPES_HH

#include <absl/container/flat_hash_map.h>
#include <tsim/label/label_dictionary.h>
#include <tsim/label/string_label.h>
#include <tsim/node/node.h>
#include <tsim/cost_model/unit_cost_model.h>

#if __cplusplus > 201703L
#include <span>
#else
#include <boost/core/span.hpp>
#endif

#include <absl/container/btree_map.h>

#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace types {

template <class K, class V>
using HashTable = absl::flat_hash_map<K, V>;
// using HashTable = std::unordered_map<K,V>;
template <class K, class V>
using TreeMTable = absl::btree_multimap<K, V>;

#if __cplusplus > 201703L
template <class K>
using span = std::span<K>;
#else
template <class K>
using span = boost::span<K>;
#endif

enum DatatypeId { SET, STRING, TREE };

class Data {
public:
  using Id = int64_t;
  static const Id INVALID = -1;
  Id id;

  explicit Data(Id id) : id(id) {}
};

using ResultPair = std::pair<Data::Id, Data::Id>;
using ResultPairs = std::vector<ResultPair>;

template <class T>
class Meta {};

template <class T>
class DataMeta {
public:
  using value_type = T;
  std::vector<value_type> data;
  Meta<T> meta;
};

template <class T>
class DataBatch {
public:
  using value_type = T;
  span<value_type> data;
  Meta<T>& meta;

public:
  explicit DataBatch(DataMeta<T>& data) : data(data.data), meta(data.meta) {}
  DataBatch(span<value_type> s, Meta<T>& meta) : data(s), meta(meta) {}
};

class Set : public Data {
public:
  using Token = int64_t;
  std::vector<Token> tokens;
  explicit Set(Id id) : Data(id) {}
  Set() : Data(INVALID) {}

  bool operator<(const Set& rhs) const {
    if (tokens.size() != rhs.tokens.size()) {
      return tokens.size() < rhs.tokens.size();
    }
    return tokens < rhs.tokens;
  }
};

template <>
class Meta<Set> {};

using Sets = DataMeta<Set>;
using SetBatch = DataBatch<Set>;

inline std::ostream& operator<<(std::ostream& os, const Set& obj) {
  os << "(" << obj.id << ", [";
  for (auto token : obj.tokens) {
    os << token << ", ";
  }
  os << "])";
  return os;
}

class String : public Data {
public:
  // we have to use "longer" strings here as a reduction to strings might result in requiring more than 8 bits for each
  // character
  using str_t = std::u32string;
  str_t str;

  String() : Data(INVALID) {}
  // todo remove
  String(const Id id, str_t str) : Data(id), str(std::move(str)) {}
  String(const Id id, const std::string& str) : Data(id), str(str.begin(), str.end()) {}

  bool operator<(const String& rhs) const {
    if (str.size() != rhs.str.size()) {
      return str.size() < rhs.str.size();
    }
    return str < rhs.str;
  }
};
template <>
class Meta<String> {};
using Strings = DataMeta<String>;
using StringBatch = DataBatch<String>;

inline std::ostream& operator<<(std::ostream& os, const String& obj) {
  os << "(" << obj.id << ", [";
  for (auto c : obj.str) {
    if (c > std::numeric_limits<char>::max()) {
      os << static_cast<int32_t>(c) << ", ";
    } else {
      os << static_cast<char>(c);
    }
  }
  os << "])";
  return os;
}

// todo add constructors
class Tree : public Data {
public:
  using Label = tsim::label::StringLabel;
  using Node = tsim::node::Node<Label>;
  using LabelDictionary = tsim::label::LabelDictionary<Label>;
  using CostModel = tsim::cost_model::UnitCostModelLD<Label>;

public:
  explicit Tree(Id id, Node&& node) : Data(id), root(std::forward<Node>(node)) {}

public:
  Node root;
};
template <>
class Meta<Tree> {
public:
  Tree::LabelDictionary label_dict;
  Tree::CostModel cost_model{label_dict};
};
using Trees = DataMeta<Tree>;
using TreeBatch = DataBatch<Tree>;

inline std::ostream& operator<<([[maybe_unused]] std::ostream& os, [[maybe_unused]] const Tree& obj) {
  os << "(" << obj.id << ", [";
  std::vector<std::string> labels;
  obj.root.get_all_labels_recursion(labels);
  for (auto& str : labels) {
    os << str << ", ";
  }
  os << "])";
  return os;
}

using Dataset = std::variant<Sets, Strings, Trees>;
using Batch = std::variant<SetBatch, StringBatch, TreeBatch>;

inline void print_result_pairs(std::ostream& ostream, ResultPairs& pairs, Dataset& data) {
  std::visit(
    [&](auto& actual_dataset) {
      for (auto [id1, id2] : pairs) {
        auto& o1 = actual_dataset.data[id1];
        auto& o2 = actual_dataset.data[id2];

        ostream << "(" << o1 << " : " << o2 << ")" << std::endl;
      }
    },
    data);
}

}  // namespace types

#endif  // SRC_TYPES_HH
