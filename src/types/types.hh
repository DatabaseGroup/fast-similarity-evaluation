#ifndef SRC_TYPES_HH
#define SRC_TYPES_HH

#include <absl/container/flat_hash_map.h>

#if __cplusplus > 201703L
#include <span>
#else
#include <boost/core/span.hpp>
#endif

#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace types {

template <class K, class V>
// using HashTable = absl::flat_hash_map<K, V>;
using HashTable = std::unordered_map<K,V>;

#if __cplusplus > 201703L
template<class K>
using span = std::span<K>;
#else
template<class K>
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

class Set : public Data {
public:
  using Token = int64_t;
  std::vector<Token> tokens;
  explicit Set(Id id) : Data(id) {}
  Set() : Data(INVALID) {}
};
using Sets = std::vector<Set>;
using SetBatch = span<Set>;

std::ostream& operator<<(std::ostream& os, const Set& obj)
{
  os << "(" << obj.id << ", [";
  for (auto token : obj.tokens) {
    os << token << ", ";
  }
  os << "])";
  return os;
}

class String : public Data {
public:
  std::string str;

  String() : Data(INVALID) {}
  // todo remove
  String(Id id, std::string str) : Data(id), str(std::move(str)) {}
  String(Id id, const char* s, const std::streamsize n) : Data(id), str(s, n) {}
};
using Strings = std::vector<String>;
using StringBatch = span<String>;

std::ostream& operator<<(std::ostream& os, const String& obj)
{
  os << "(" << obj.id << ", [";
  os << obj.str;
  os << "])";
  return os;
}

// todo add constructors
class Tree : public Data {
public:
  explicit Tree() : Data(INVALID) {}
};
using Trees = std::vector<Tree>;
using TreeBatch = span<Tree>;

std::ostream& operator<<([[maybe_unused]]std::ostream& os, [[maybe_unused]] const Tree& obj)
{
  throw std::invalid_argument("Printing trees is not implemented yet. Maybe do bracket notation? See tree-edit library");
}

using Dataset = std::variant<Sets, Strings, Trees>;
using Batch = std::variant<SetBatch, StringBatch, TreeBatch>;

Batch dataset_to_batch(Dataset& dataset) {
  return std::visit([](auto&& data){
    using DatasetType = std::decay_t<decltype(data)>;
    return Batch(span<typename DatasetType::value_type>(data));
  }, dataset);
}

Batch get_batch(Dataset& dataset, const int64_t batch_idx, const int64_t batch_size) {
  int64_t offset = batch_idx * batch_size;
  return std::visit([&](auto&& data){
    using DatasetType = std::decay_t<decltype(data)>;
    return Batch(span<typename DatasetType::value_type>(data.begin() + offset, std::min(data.begin() + offset + batch_size, data.end())));
  }, dataset);
}

void print_result_pairs(std::ostream& ostream, ResultPairs& pairs, Dataset& data) {
  std::visit([&](auto& data) {
    for (auto [id1, id2] : pairs) {
      auto& o1 = data[id1];
      auto& o2 = data[id2];

      ostream << "(" << o1 << " : " << o2 << ")" << std::endl;
    }
  }, data);
}

}  // namespace types

#endif  // SRC_TYPES_HH
