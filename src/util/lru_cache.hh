#ifndef SRC_LRU_CACHE_HH
#define SRC_LRU_CACHE_HH

#include "../types/types.hh"

namespace util {

// inspired by (i.e., stolen from) https://www.boost.org/doc/libs/1_84_0/boost/compute/detail/lru_cache.hpp

template<class Key, class Value>
class LRUCache {
public:
  explicit LRUCache(size_t capacity) : cache_capacity(capacity) {}

public:
  void reserve(size_t n) {
    while (capacity() <= n + size()) {
      evict();
    }
  }

  [[nodiscard]] size_t size() const {
    return values.size();
  }

  [[nodiscard]] size_t capacity() const {
    return cache_capacity;
  }

  [[nodiscard]] bool empty() const {
    return values.empty();
  }

  [[nodiscard]] bool contains(const Key& key) const {
    return values.find(key) != values.end();
  }

  void clear() {
    values.clear();
    access_list.clear();
  }

  Value& emplace(const Key& key, auto&&... value) {
    // do not insert if key already exists
    auto it = values.find(key);
    if (it == values.end()) {
      if (size() >= capacity()) {
        evict();
      }

      // just accessed, put in front
      access_list.push_front(key);
      auto value_it = values.try_emplace(key, std::make_pair(Value(std::forward<decltype(value)>(value)...), access_list.begin()));

      // my eyes hurt from this
      return value_it.first->second.first;
    }
    return it->second.first;
  }

  std::optional<Value> get(const Key& key) {
    auto it = values.find(key);

    // if not found, return none
    if (it == values.end()) {
      // this is none
      return std::nullopt;
    }

    // value exists, but first update its last access time
    auto list_it = it->second.second;
    // if item not first in list...
    if (list_it != access_list.begin()) {
      // ...put it there by removing, pushing, and updating the reference in the table
      access_list.erase(list_it);
      access_list.push_front(key);
      it->second.second = access_list.begin();
    }
    return std::make_optional<Value>(it->second.first);
  }

private:
  void evict() {
    // get least recently used key (or iterator on that key)
    auto last_element_it = --access_list.end();
    // remove from table and list (value for table, "pointer" for list)
    values.erase(*last_element_it);
    access_list.erase(last_element_it);
  }

private:
  std::list<Key> access_list;
  types::HashTable<Key, std::pair<Value, typename std::list<Key>::iterator>> values;
  size_t cache_capacity;
};

}

#endif  // SRC_LRU_CACHE_HH
