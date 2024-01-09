#ifndef SRC_TYPES_HH
#define SRC_TYPES_HH

#include <absl/container/flat_hash_map.h>

#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace types {

template <class K, class V>
using HashTable = absl::flat_hash_map<K, V>;

enum DatatypeId { SET, STRING, TREE };

class Data {
public:
  using Id = size_t;
  Id id;

  explicit Data(Id id) : id(id) {}
};

using ResultPair = std::pair<Data::Id, Data::Id>;

class Set : public Data {
public:
  using Token = int64_t;
  std::vector<Token> tokens;
  explicit Set(Id id) : Data(id) {}
};
using Sets = std::vector<Set>;

class String : public Data{
public:
  std::string str;

  // todo remove
  String(Id id, std::string str) : Data(id), str(std::move(str)) {}
  String(Id id, const char* s, const std::streamsize n) : Data(id), str(s, n) {}
};
using Strings = std::vector<String>;

class Tree : public Data {};
using Trees = std::vector<Tree>;

using Dataset = std::variant<Sets, Strings, Trees>;

}  // namespace types

#endif  // SRC_TYPES_HH
