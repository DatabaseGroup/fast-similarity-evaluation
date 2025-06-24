#ifndef SRC_INDEX_HH
#define SRC_INDEX_HH

#include "absl/container/btree_map.h"
#include "absl/container/flat_hash_map.h"

namespace indexing {

enum IndexType {
  HASH,  // only direct access is allowed, universe of keys unknown
  ORDERED_PRESORTED,  // range queries for one dimension are allowed, data inserted is already sorted
  ORDERED_RANDOM  // range queries for one dimension are allowed, random order of inserts supported
};

using KeyType = int64_t;
using KeyRange = std::pair<KeyType, KeyType>;

// "Interface" of index iterator
class KeyIterator {
  template <int32_t LEVEL, class DUMMY = void>
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
  template <int32_t LEVEL>
  void set_level_key(KeyType key) {
    IteratorHolder<LEVEL>::set_level_key(*this, key);
  }

  template <int32_t LEVEL>
  typename IteratorHolder<LEVEL>::iter get_level_iterator() {
    IteratorHolder<LEVEL>::get_level_iterator(*this);
  }

  template <int32_t LEVEL>
  typename IteratorHolder<LEVEL>::iter get_level_end() {
    IteratorHolder<LEVEL>::get_level_end(*this);
  }
};

// arguably somewhat terrible code, but highly customizable due to the templates
// specializing a templated method of a templated class is terrible (and only works
// for partial specialization for some arcane reason)
template <class KeyClass>
class StaticKeyIterator {
  // the dummy only exists to make specializations partial instead of explicit (i.e., full)
  template <int32_t LEVEL, class DUMMY = void>
  struct IteratorHolder {};

  template <class DUMMY>
  struct IteratorHolder<0, DUMMY> {
    using iter = std::array<KeyClass, 1>::const_iterator;

    static void set_level_key([[maybe_unused]] StaticKeyIterator& iterator, [[maybe_unused]] KeyType key) {
      // nop
    }

    static iter get_level_iterator(StaticKeyIterator& iterator) { return iterator.stored_key.begin(); }

    static iter get_level_end(StaticKeyIterator& iterator) { return iterator.stored_key.end(); }
  };

public:
  explicit StaticKeyIterator(KeyClass key) : stored_key({key}) {}

public:
  template <int32_t LEVEL>
  void set_level_key([[maybe_unused]] KeyType key) {
    return IteratorHolder<LEVEL>::set_level_key(*this, key);
  }

  template <int32_t LEVEL>
  typename IteratorHolder<LEVEL>::iter get_level_iterator() {
    return IteratorHolder<LEVEL>::get_level_iterator(*this);
  }

  template <int32_t LEVEL>
  typename IteratorHolder<LEVEL>::iter get_level_end() {
    return IteratorHolder<LEVEL>::get_level_end(*this);
  }

public:
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
  template <class CallbackFun, class KeyFun, int32_t LEVEL = 0>
  void query(KeyType key, CallbackFun callback, [[maybe_unused]] KeyFun& key_function) {
    auto iter = map.find(key);

    if (iter != map.end()) {
      auto& vec = iter->second;
      for (auto entry : vec) {
        if (callback(entry)) {
          break;
        }
      }
    }
  }

  void insert(ValueType value, KeyType key) { map[key].emplace_back(value); }

  void merge(ComplexIndex& other, ValueType id_offset) {
    for (auto& [key, value] : other.map) {
      auto& list = map[key];
      for (auto val : value) {
        list.push_back(val + id_offset);
      }
    }
  }

  void clear() { map.clear(); }

  static constexpr int32_t LEVEL() { return 0; }

public:
  types::HashTable<KeyType, std::vector<ValueType>> map;
};

template <class ValueType, IndexType... TailIndexes>
class ComplexIndex<ValueType, HASH, TailIndexes...> {
public:
  template <class CallbackFun, class KeyFun, int32_t LEVEL = 0>
  void query(KeyType key, CallbackFun callback, KeyFun& key_function) {
    auto it = map.find(key);

    if (it != map.end()) {
      auto& inner_index = it->second;

      key_function.template set_level_key<LEVEL>(key);
      for (auto next_key_iter = key_function.template get_level_iterator<LEVEL>();
           next_key_iter != key_function.template get_level_end<LEVEL>();
           ++next_key_iter) {
        auto next_key = *next_key_iter;
        inner_index.template query<CallbackFun, KeyFun, LEVEL + 1>(next_key, callback, key_function);
      }
    }
  }

  template <class... Keys>
  void insert(ValueType value, KeyType key, Keys... keys) {
    map[key].insert(value, keys...);
  }

  void merge(ComplexIndex& other, ValueType id_offset) {
    for (auto& [key, value] : other.map) {
      auto& index = map[key];
      index.merge(value, id_offset);
    }
  }

  void clear() { map.clear(); }

  static constexpr int32_t LEVEL() { return ComplexIndex<ValueType, TailIndexes...>::LEVEL() + 1; }

public:
  types::HashTable<KeyType, ComplexIndex<ValueType, TailIndexes...>> map;
};

template <class ValueType>
class ComplexIndex<ValueType, ORDERED_PRESORTED> {
public:
  template <class CallbackFun, class KeyFun, int32_t LEVEL = 0>
  void query(KeyRange key_range, CallbackFun callback, [[maybe_unused]] KeyFun& key_function) {
    auto key_begin = key_range.first;
    auto key_end = key_range.second;

    auto iter = find(key_begin);

    for (; iter != map.end(); ++iter) {
      if (iter->first > key_end) {
        break;
      }
      if (callback(iter->second)) {
        break;
      }
    }
  }

  void insert(ValueType value, KeyType key) {
    assert(map.empty() || map.back().first <= key);
    map.emplace_back(key, value);
  }

  void merge(ComplexIndex& other, ValueType id_offset) {
    for (auto& [key, value] : other.map) {
      map.emplace_back(key, value + id_offset);
    }
  }

  void clear() { map.clear(); }

  static constexpr int32_t LEVEL() { return 0; }

public:
  std::vector<std::pair<KeyType, ValueType>> map;

private:
  typename decltype(map)::iterator find(KeyType key) {
    return std::lower_bound(map.begin(), map.end(), key, [](const auto& p, int key) -> bool { return p.first < key; });
  }
};

template <class ValueType>
class ComplexIndex<ValueType, ORDERED_RANDOM> {
public:
  template <class CallbackFun, class KeyFun, int32_t LEVEL = 0>
  void query(KeyRange key_range, CallbackFun callback, [[maybe_unused]] KeyFun& key_function) {
    auto key_begin = key_range.first;
    auto key_end = key_range.second;

    auto iter = map.lower_bound(key_begin);

    for (; iter != map.end(); ++iter) {
      if (iter->first > key_end) {
        break;
      }
      for (auto& val : iter->second) {
        if (callback(val)) {
          break;
        }
      }
    }
  }

  void insert(ValueType value, KeyType key) { map[key].emplace_back(value); }

  void merge(ComplexIndex& other, ValueType id_offset) {
    for (auto& [key, value] : other.map) {
      insert(key, value + id_offset);
    }
  }

  void clear() { map.clear(); }

  static constexpr int32_t LEVEL() { return 0; }

public:
  absl::btree_map<KeyType, std::vector<ValueType>> map;
};

template <class ValueType, IndexType... TailIndexes>
class ComplexIndex<ValueType, ORDERED_PRESORTED, TailIndexes...> {
private:
  using KeyIndexPair = std::pair<KeyType, ComplexIndex<ValueType, TailIndexes...>>;

public:
  template <class CallbackFun, class KeyFun, int32_t LEVEL = 0>
  void query(KeyRange key_range, CallbackFun callback, KeyFun& key_function) {
    auto key_begin = key_range.first;
    auto key_end = key_range.second;

    auto iter = find(key_begin);

    for (; iter != map.end(); ++iter) {
      if (iter->first > key_end) {
        break;
      } else {
        auto key = iter->first;
        key_function.template set_level_key<LEVEL>(key);
        for (auto next_key_iter = key_function.template get_level_iterator<LEVEL>();
             next_key_iter != key_function.template get_level_end<LEVEL>();
             ++next_key_iter) {
          auto next_key = *next_key_iter;
          iter->second.template query<CallbackFun, KeyFun, LEVEL + 1>(next_key, callback, key_function);
        }
      }
    }
  }

  ComplexIndex<ValueType, TailIndexes...>& get(KeyType key) {
    auto it = find(key);
    if (it == map.end()) {
      assert(map.empty() || map.back().first < key);
      map.emplace_back(std::make_pair(key, ComplexIndex<ValueType, TailIndexes...>()));
      return map.back().second;
    } else {
      return it->second;
    }
  }

  template <class... Keys>
  void insert(ValueType value, KeyType key, Keys... keys) {
    if (map.empty() || map.back().first < key) {
      map.emplace_back(key, ComplexIndex<ValueType, TailIndexes...>{});
    }
    map.back().second.insert(value, keys...);
  }

  void merge([[maybe_unused]] ComplexIndex& other, [[maybe_unused]] ValueType id_offset) {
    // unsupported for ORDERED_PRESORTED
    throw std::logic_error("merge() is not implemented for ORDERED_PRESORTED.");
  }

  void clear() { map.clear(); }

  static constexpr int32_t LEVEL() { return ComplexIndex<ValueType, TailIndexes...>::LEVEL() + 1; }

public:
  std::vector<std::pair<KeyType, ComplexIndex<ValueType, TailIndexes...>>> map;

private:
  typename decltype(map)::iterator find(KeyType key) {
    return std::lower_bound(map.begin(), map.end(), key, [](const auto& p, int key) -> bool { return p.first < key; });
  }
};

template <class ValueType, IndexType... TailIndexes>
class ComplexIndex<ValueType, ORDERED_RANDOM, TailIndexes...> {
private:
  using KeyIndexPair = std::pair<KeyType, ComplexIndex<ValueType, TailIndexes...>>;

public:
  template <class CallbackFun, class KeyFun, int32_t LEVEL = 0>
  void query(KeyRange key_range, CallbackFun callback, KeyFun& key_function) {
    auto key_begin = key_range.first;
    auto key_end = key_range.second;

    auto iter = map.lower_bound(key_begin);

    for (; iter != map.end(); ++iter) {
      if (iter->first > key_end) {
        break;
      } else {
        auto key = iter->first;
        key_function.template set_level_key<LEVEL>(key);
        for (auto next_key_iter = key_function.template get_level_iterator<LEVEL>();
             next_key_iter != key_function.template get_level_end<LEVEL>();
             ++next_key_iter) {
          auto next_key = *next_key_iter;
          iter->second.template query<CallbackFun, KeyFun, LEVEL + 1>(next_key, callback, key_function);
        }
      }
    }
  }

  ComplexIndex<ValueType, TailIndexes...>& get(KeyType key) { return map[key]; }

  template <class... Keys>
  void insert(ValueType value, KeyType key, Keys... keys) {
    map[key].insert(value, keys...);
  }

  void merge(ComplexIndex& other, ValueType id_offset) {
    for (auto& [key, value] : other.map) {
      auto& index = map[key];
      index.merge(value, id_offset);
    }
  }

  void clear() { map.clear(); }

  static constexpr int32_t LEVEL() { return ComplexIndex<ValueType, TailIndexes...>::LEVEL() + 1; }

public:
  absl::btree_map<KeyType, ComplexIndex<ValueType, TailIndexes...>> map;
};

template <class ValueType, IndexType HeadIndex, IndexType... TailIndexes>
class ComplexIndex<ValueType, HeadIndex, TailIndexes...> {
private:
  // Index<HeadIndex, ComplexIndex<ValueType, TailIndexes...>> inner_index;
};

}  // namespace indexing

#endif  // SRC_INDEX_HH
