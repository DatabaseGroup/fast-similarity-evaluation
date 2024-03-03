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

// todo make this a concept?
/*class KeyFunction {
  class KeyFunctionIterator {

  };

  // only single key added to hierarchy
  KeyFunctionIterator begin() {

  }

  KeyFunctionIterator end() {

  }
};*/

class StaticNextKeyFunction {
public:
  explicit StaticNextKeyFunction(KeyType key) : stored_key({key}) {}

public:
  [[nodiscard]] std::array<KeyType, 1>::const_iterator begin([[maybe_unused]] const KeyType previous_key) const {
    return stored_key.begin();
  }

  [[nodiscard]] std::array<KeyType, 1>::const_iterator end() const { return stored_key.end(); }

private:
  std::array<KeyType, 1> stored_key;
};

using KeyRange = std::pair<KeyType, KeyType>;
class StaticNextKeyRange {
public:
  explicit StaticNextKeyRange(KeyType lower, KeyType upper) : stored_key_range({std::make_pair(lower, upper)}) {}

public:
  [[nodiscard]] std::array<KeyRange, 1>::const_iterator begin([[maybe_unused]] const KeyType previous_key) const {
    return stored_key_range.begin();
  }

  [[nodiscard]] std::array<KeyRange, 1>::const_iterator end() const { return stored_key_range.end(); }

private:
  std::array<KeyRange, 1> stored_key_range;
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

      for (auto next_key_iter = next_key_fun.begin(key); next_key_iter != next_key_fun.end(); ++next_key_iter) {
        auto next_key = *next_key_iter;
        inner_index.query(next_key, callback, key_funs...);
      }
    }
  }

  template <class... Keys>
  void insert(ValueType value, KeyType key, Keys... keys) {
    map[key].insert(value, keys...);
  }

private:
  types::HashTable<KeyType, ComplexIndex<ValueType, TailIndexes...>> map;
};

int32_t log2(uint32_t x) { return (31 - __builtin_clz(x)); }

template <class ValueType>
class ComplexIndex<ValueType, DISCRETE> {
public:
  explicit ComplexIndex(size_t max_key) : map(max_key + 1) {}
  ComplexIndex() = default;

public:
  template <class CallbackFun>
  void query(KeyType key, CallbackFun callback) {
    if (key < map.size() && 0 <= key) {
      auto& vec = map[key];
      for (auto entry : vec) {
        callback(entry);
      }
    }
  }

  void insert(ValueType value, KeyType key) {
    // todo make this toggleable
    if (map.size() <= key) {
      auto new_size = 1ul << (log2(key + 1) + 1);
      map.resize(new_size);
    }
    map[key].emplace_back(value);
  }

private:
  std::vector<std::vector<ValueType>> map;
};

template <class ValueType, IndexType... TailIndexes>
class ComplexIndex<ValueType, DISCRETE, TailIndexes...> {
public:
  explicit ComplexIndex(int64_t max_key) : map(max_key + 1) {}
  ComplexIndex() = default;

public:
  template <class CallbackFun, class KeyFun, class... KeyFunTail>
  void query(KeyType key, CallbackFun callback, KeyFun next_key_fun, KeyFunTail... key_funs) {
    if (key < map.size() && 0 <= key) {
      auto& inner_index = map[key];

      for (auto next_key_iter = next_key_fun.begin(key); next_key_iter != next_key_fun.end(); ++next_key_iter) {
        auto next_key = *next_key_iter;
        inner_index.query(next_key, callback, key_funs...);
      }
    }
  }

  template <class... Keys>
  void insert(ValueType value, KeyType key, Keys... keys) {
    // todo make this toggleable
    // in any case faster than hashing
    if (map.size() <= key) {
      auto new_size = 1ul << (log2(key + 1) + 1);
      map.resize(new_size);
    }
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
        for (auto next_key_iter = next_key_fun.begin(iter->first); next_key_iter != next_key_fun.end();
             ++next_key_iter) {
          auto next_key = *next_key_iter;
          iter->second.query(next_key, callback, key_funs...);
        }
      }
    }
  }

  template <class... Keys>
  void insert(ValueType value, KeyType key, Keys... keys) {
    // assume insertions are in order
    assert(map.empty() || map.back().first <= key);
    if (map.empty() || map.back().first != key) {
      map.emplace_back();
      map.back().first = key;
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
