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
using KeyRange = std::pair<KeyType, KeyType>;

// "Interface" of index iterator
class KeyIterator {
  template<int32_t LEVEL, class DUMMY = void>
  class IteratorHolder {
    // using iter = ...

    static void set_level_key([[maybe_unused]] KeyIterator& iterator, [[maybe_unused]] KeyType key) {
      // ...
    }
    /*
    static iter get_level_iterator(KeyIterator& iterator) {
      // ...
    }

    static iter get_level_end(KeyIterator& iterator) {
      // ...
    }
     */
  };

public:
  template<int32_t LEVEL>
  void set_level_key(KeyType key) {
    IteratorHolder<LEVEL>::set_level_key(*this, key);
  }

  template<int32_t LEVEL>
  typename IteratorHolder<LEVEL>::iter get_level_iterator() {
    IteratorHolder<LEVEL>::get_level_iterator(*this);
  }

  template<int32_t LEVEL>
  typename IteratorHolder<LEVEL>::iter get_level_end() {
    IteratorHolder<LEVEL>::get_level_end(*this);
  }
};

// arguably somewhat terrible code, but highly customizable due to the templates
// specializing a templated method of a templated class is terrible (and only works
// for partial specialization for some arcane reason)
template<class KeyClass>
class StaticKeyIterator {
  // the dummy only exists to make specializations partial instead of explicit (i.e., full)
  template<int32_t LEVEL, class DUMMY = void>
  struct IteratorHolder {};

  template<class DUMMY>
  struct IteratorHolder<0, DUMMY> {
    using iter = std::array<KeyClass, 1>::const_iterator;

    static void set_level_key([[maybe_unused]] StaticKeyIterator& iterator, [[maybe_unused]] KeyType key) {
      // nop
    }

    static iter get_level_iterator(StaticKeyIterator& iterator) {
      return iterator.stored_key.begin();
    }

    static iter get_level_end(StaticKeyIterator& iterator) {
      return iterator.stored_key.end();
    }
  };

public:
  explicit StaticKeyIterator(KeyClass key) : stored_key({key}) {}

public:
  template<int32_t LEVEL>
  void set_level_key([[maybe_unused]]KeyType key) {
    return IteratorHolder<LEVEL>::set_level_key(*this, key);
  }

  template<int32_t LEVEL>
  typename IteratorHolder<LEVEL>::iter get_level_iterator() {
    return IteratorHolder<LEVEL>::get_level_iterator(*this);
  }

  template<int32_t LEVEL>
  typename IteratorHolder<LEVEL>::iter get_level_end() {
    return IteratorHolder<LEVEL>::get_level_end(*this);
  }

private:
  std::array<KeyClass, 1> stored_key;
};

using StaticPointIterator = StaticKeyIterator<KeyType>;
using StaticRangeIterator = StaticKeyIterator<KeyRange>;

// curried functions are possible by letting them have shared state (the first function sets the first parameter and
// returns the loosest bound etc.)

template <class ValueType, IndexType... IndexTypes>
class ComplexIndex {
  // typical use:
  // insert(set_id, list, of, keys, in, each, dimension);
};

template <class ValueType>
class ComplexIndex<ValueType, HASH> {
public:
  template <class CallbackFun, class KeyFun, int32_t LEVEL=0>
  void query(KeyType key, CallbackFun callback, [[maybe_unused]] KeyFun& key_function) {
    auto iter = map.find(key);

    if (iter != map.end()) {
      auto& vec = iter->second;
      for (auto entry : vec) {
        callback(entry);
      }
    }
  }

  void insert(ValueType value, KeyType key) {
    map[key].emplace_back(value);
  }

  static constexpr int32_t LEVEL() {
    return 0;
  }

private:
  types::HashTable<KeyType, std::vector<ValueType>> map;
};

template <class ValueType, IndexType... TailIndexes>
class ComplexIndex<ValueType, HASH, TailIndexes...> {
public:
  template <class CallbackFun, class KeyFun, int32_t LEVEL=0>
  void query(KeyType key, CallbackFun callback, KeyFun& key_function) {
    auto it = map.find(key);

    if (it != map.end()) {
      auto& inner_index = it->second;

      key_function.template set_level_key<LEVEL>(key);
      for (auto next_key_iter = key_function.template get_level_iterator<LEVEL>(); next_key_iter != key_function.template get_level_end<LEVEL>(); ++next_key_iter) {
        auto next_key = *next_key_iter;
        inner_index.template query<CallbackFun, KeyFun, LEVEL + 1>(next_key, callback, key_function);
      }
    }
  }

  template <class... Keys>
  void insert(ValueType value, KeyType key, Keys... keys) {
    map[key].insert(value, keys...);
  }

  static constexpr int32_t LEVEL() {
    return ComplexIndex<ValueType, TailIndexes...>::LEVEL() + 1;
  }

private:
  types::HashTable<KeyType, ComplexIndex<ValueType, TailIndexes...>> map;
};

// fast floor(log_2(x)) using bit-representation
// log_2(x) is (0-indexed) position of highest bit set to 1 in x
inline int32_t log2(uint32_t x) { return (31 - __builtin_clz(x)); }

template <class ValueType>
class ComplexIndex<ValueType, DISCRETE> {
public:
  explicit ComplexIndex(size_t max_key) : map(max_key + 1) {}
  ComplexIndex() = default;

public:
  template <class CallbackFun, class KeyFun, int32_t LEVEL=0>
  void query(KeyType key, CallbackFun callback, [[maybe_unused]] KeyFun& key_function) {
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

  static constexpr int32_t LEVEL() {
    return 0;
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
  template <class CallbackFun, class KeyFun, int32_t LEVEL=0>
  void query(KeyType key, CallbackFun callback, KeyFun& key_function) {
    if (key < static_cast<int64_t>(map.size()) && 0 <= key) {
      auto& inner_index = map[key];

      key_function.template set_level_key<LEVEL>(key);
      for (auto next_key_iter = key_function.template get_level_iterator<LEVEL>(); next_key_iter != key_function.template get_level_end<LEVEL>(); ++next_key_iter) {
        auto next_key = *next_key_iter;
        inner_index.template query<CallbackFun, KeyFun, LEVEL + 1>(next_key, callback, key_function);
      }
    }
  }

  template <class... Keys>
  void insert(ValueType value, KeyType key, Keys... keys) {
    // todo make this toggleable
    // in any case faster than hashing
    if (static_cast<int64_t>(map.size()) <= key) {
      auto new_size = 1ul << (log2(key + 1) + 1);
      map.resize(new_size);
    }
    map[key].insert(value, keys...);
  }

  static constexpr int32_t LEVEL() {
    return ComplexIndex<ValueType, TailIndexes...>::LEVEL() + 1;
  }

private:
  std::vector<ComplexIndex<ValueType, TailIndexes...>> map;
};

template <class ValueType>
class ComplexIndex<ValueType, ORDERED> {
private:
  using KeyValuePair = std::pair<KeyType, ValueType>;

public:
  template <class CallbackFun, class KeyFun, int32_t LEVEL=0>
  void query(KeyRange key_range, CallbackFun callback, [[maybe_unused]] KeyFun& key_function) {
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

  static constexpr int32_t LEVEL() {
    return 0;
  }

private:
  std::vector<KeyValuePair> map;
};

template <class ValueType, IndexType... TailIndexes>
class ComplexIndex<ValueType, ORDERED, TailIndexes...> {
private:
  using KeyIndexPair = std::pair<KeyType, ComplexIndex<ValueType, TailIndexes...>>;

public:
  template <class CallbackFun, class KeyFun, int32_t LEVEL=0>
  void query(KeyRange key_range, CallbackFun callback, KeyFun& key_function) {
    auto key_begin = key_range.first;
    auto key_end = key_range.second;

    auto iter = std::lower_bound(
      map.begin(), map.end(), key_begin, [](const KeyIndexPair& o1, const KeyType o2) { return o1.first < o2; });

    for (; iter != map.end(); ++iter) {
      if (iter->first > key_end) {
        break;
      } else {
        auto key = iter->first;
        key_function.template set_level_key<LEVEL>(key);
        for (auto next_key_iter = key_function.template get_level_iterator<LEVEL>(); next_key_iter != key_function.template get_level_end<LEVEL>(); ++next_key_iter) {
          auto next_key = *next_key_iter;
          iter->second.template query<CallbackFun, KeyFun, LEVEL + 1>(next_key, callback, key_function);
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

  static constexpr int32_t LEVEL() {
    return ComplexIndex<ValueType, TailIndexes...>::LEVEL() + 1;
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
