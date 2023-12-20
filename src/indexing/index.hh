#ifndef SRC_INDEX_HH
#define SRC_INDEX_HH

#include "absl/container/flat_hash_map.h"

namespace indexing {

enum IndexType {
  HASH,  // only direct access is allowed, universe of keys unknown
  DISCRETE,  // only direct access is allowed, universe of keys known, bounded, and small
  ORDERED  // range queries for one dimension are allowed
};

using KeyType = int64_t;

/*template <enum IndexType, class ValueType>
class Index {};

template <class ValueType>
class Index<HASH, ValueType> {
public:
  ValueType& operator[](KeyType key) {
    return map[key];
  }

private:
  absl::flat_hash_map<KeyType, ValueType> map;
};

template <class ValueType>
class Index<DISCRETE, ValueType> {

};

template <class ValueType>
class Index<ORDERED, ValueType> {};
*/

class StaticNextKeyFunction {
public:
  explicit StaticNextKeyFunction(KeyType key) : key(key) {}

public:
  uint64_t operator()([[maybe_unused]] const KeyType previous_key) const { return key; }

private:
  uint64_t key;
};

using KeyRange = std::pair<KeyType, KeyType>;
class StaticNextKeyRange {
public:
  explicit StaticNextKeyRange(KeyType lower, KeyType upper) : key_range(lower, upper) {}

public:
  const KeyRange& operator()([[maybe_unused]] const KeyType previous_key) const { return key_range; }

private:
  KeyRange key_range;
};

// curried functions are possible by letting them have shared state (the first function sets the first parameter and
// returns the loosest bound etc.)

// Current limitations:
// - If DISCRETE is used, it has to be the outermost index (might be fixed with passing around constructor arguments)
template <class ValueType, IndexType... IndexTypes>
class ComplexIndex {
  // typical use:
  // insert(set_id, list, of, keys, in, each, dimension);
};

template <class ValueType>
class ComplexIndex<ValueType, HASH> {
public:
  template <class CallbackFun>
  void query(KeyType key, CallbackFun callback) {
    auto iter = map.find(key);

    if (iter != map.end()) {
      auto& vec = iter->second;
      for (auto entry : vec) {
        callback(entry);
      }
    }
  }

  void insert(KeyType key, ValueType value) { map[key].emplace_back(value); }

private:
  types::HashTable<KeyType, std::vector<ValueType>> map;
};

template <class ValueType, IndexType... TailIndexes>
class ComplexIndex<ValueType, HASH, TailIndexes...> {
public:
  template <class CallbackFun, class KeyFun, class... KeyFunTail>
  void query(KeyType key, CallbackFun callback, KeyFun next_key_fun, KeyFunTail... key_funs) {
    auto it = map.find(key);

    if (it != map.end()) {
      auto& inner_index = it->second;
      auto next_key = next_key_fun(key);
      inner_index.query(next_key, callback, key_funs...);
    }
  }

  template <class... Keys>
  void insert(ValueType value, KeyType key, Keys... keys) {
    map[key].insert(value, keys...);
  }

private:
  types::HashTable<KeyType, ComplexIndex<ValueType, TailIndexes...>> map;
};

template <class ValueType>
class ComplexIndex<ValueType, DISCRETE> {
public:
  explicit ComplexIndex(size_t max_key) : map(max_key + 1) {}

public:
  template <class CallbackFun>
  void query(KeyType key, CallbackFun callback) {
    auto& vec = map[key];
    for (auto entry : vec) {
      callback(entry);
    }
  }

  void insert(ValueType value, KeyType key) { map[key].emplace_back(value); }

private:
  std::vector<std::vector<ValueType>> map;
};

template <class ValueType, IndexType... TailIndexes>
class ComplexIndex<ValueType, DISCRETE, TailIndexes...> {
public:
  explicit ComplexIndex(int64_t max_key) : map(max_key + 1) {}

public:
  template <class CallbackFun, class KeyFun, class... KeyFunTail>
  void query(KeyType key, CallbackFun callback, KeyFun next_key_fun, KeyFunTail... key_funs) {
    auto& inner_index = map[key];

    auto next_key = next_key_fun(key);

    inner_index.query(next_key, callback, key_funs...);
  }

  template <class... Keys>
  void insert(ValueType value, KeyType key, Keys... keys) {
    map[key].insert(value, keys...);
  }

private:
  std::vector<ComplexIndex<ValueType, TailIndexes...>> map;
};

template <class ValueType>
class ComplexIndex<ValueType, ORDERED> {
private:
  using KeyValuePair = std::pair<KeyType, ValueType>;

public:
  template <class CallbackFun>
  void query(KeyRange key_range, CallbackFun callback) {
    auto key_begin = key_range.first;
    auto key_end = key_range.second;

    auto iter = std::lower_bound(
      map.begin(), map.end(), key_begin, [](const KeyValuePair& o1, const KeyType o2) { return o1.first < o2; });

    for (; iter != map.end(); ++iter) {
      if (iter->first > key_end) {
        break;
      } else {
        callback(iter->second);
      }
    }
  }

  void insert(ValueType value, KeyType key) { map.emplace_back(key, value); }

private:
  std::vector<KeyValuePair> map;
};

template <class ValueType, IndexType... TailIndexes>
class ComplexIndex<ValueType, ORDERED, TailIndexes...> {
private:
  using KeyIndexPair = std::pair<KeyType, ComplexIndex<ValueType, TailIndexes...>>;

public:
  template <class CallbackFun, class KeyFun, class... KeyFunTail>
  void query(KeyRange key_range, CallbackFun callback, KeyFun next_key_fun, KeyFunTail... key_funs) {
    auto key_begin = key_range.first;
    auto key_end = key_range.second;

    auto iter = std::lower_bound(
      map.begin(), map.end(), key_begin, [](const KeyIndexPair& o1, const KeyType o2) { return o1.first < o2; });

    for (; iter != map.end(); ++iter) {
      if (iter->first > key_end) {
        break;
      } else {
        auto next_key = next_key_fun(iter->first);
        iter->second.query(next_key, callback, key_funs...);
      }
    }
  }

  template <class... Keys>
  void insert(ValueType value, KeyType key, Keys... keys) {
    // assume insertions are in order
    assert(map.back().first <= key);
    if (map.back().first != value) {
      map.emplace_back();
    }
    map.back().second.insert(value, keys...);
  }

private:
  std::vector<KeyIndexPair> map;
};

template <class ValueType, IndexType HeadIndex, IndexType... TailIndexes>
class ComplexIndex<ValueType, HeadIndex, TailIndexes...> {
private:
  // Index<HeadIndex, ComplexIndex<ValueType, TailIndexes...>> inner_index;
};

}  // namespace indexing

#endif  // SRC_INDEX_HH
