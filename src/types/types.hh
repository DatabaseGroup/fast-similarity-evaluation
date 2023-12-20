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

enum DATATYPE_ID { SET, STRING, TREE };

class Set {
public:
  using Token = int64_t;
  std::vector<Token> tokens;
};
using Sets = std::vector<Set>;

class String {
public:
  std::string str;

  explicit String(std::string str) : str(std::move(str)) {}
};
using Strings = std::vector<String>;

class Tree {};
using Trees = std::vector<Tree>;

using Dataset = std::variant<Sets, Strings, Trees>;

}  // namespace types

#endif  // SRC_TYPES_HH
