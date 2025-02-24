#ifndef SRC_PROJECT_STATISTICS_HH
#define SRC_PROJECT_STATISTICS_HH

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <ostream>
#include <sstream>
#include <limits>
#include <nlohmann/json.hpp>

namespace statistics {

// The MIT License (MIT)
// Copyright (c) 2018 Willi Mann
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#ifndef STATISTICS
struct RealIncreaser {
  template <class T>
  inline static void inc(T) {}
  template <class T>
  inline static void add(T, T) {}
  template <class T>
  inline static void set([[maybe_unused]] T& val, [[maybe_unused]] T a) {}
  template <class F>
  inline static void update([[maybe_unused]] const F& update_function) {}
};
#else
struct RealIncreaser {
  template <class T>
  inline static void inc(T& val) {
    val += 1;
  }
  template <class T>
  inline static void add(T& val, T a) {
    val += a;
  }
  template <class T>
  inline static void set(T& val, T a) {
    val = a;
  }
  template <class F>
  inline static void update(const F& update_function) {
    update_function();
  }
};
#endif

template <typename Increaser = RealIncreaser>
struct CountItem {
  int64_t value;
  inline void inc() { Increaser::inc(value); }
  inline void add(int64_t a) { Increaser::add(value, a); }
  inline void add_to_json(const std::string& name, nlohmann::json& json) const { json[name] = value; }
  inline void reset() { this->value = 0; }
  CountItem() : value(0) {}
};

template <typename Increaser = RealIncreaser>
struct AvgItem {
  int64_t sum;
  int64_t count;
  int64_t min;
  int64_t max;
  inline void record(int64_t a) {
    Increaser::inc(count);
    Increaser::add(sum, a);
    Increaser::set(min, std::min(min, a));
    Increaser::set(max, std::max(max, a));
  }
  [[nodiscard]] inline double avg() const {
    return count != 0 ? static_cast<double>(sum) / static_cast<double>(count) : 0;
  }
  inline void add_to_json(const std::string& name, nlohmann::json& json) const {
    json[name] = nullptr;
    json[name]["sum"] = sum;
    json[name]["count"] = count;
    json[name]["min"] = min;
    json[name]["max"] = max;
    json[name]["avg"] = avg();
  }
  inline void reset() {
    this->sum = 0;
    this->count = 0;
    this->min = std::numeric_limits<int64_t>::max();
    this->max = std::numeric_limits<int64_t>::min();
  }
  AvgItem() : sum(0), count(0), min(std::numeric_limits<int64_t>::max()), max(std::numeric_limits<int64_t>::min()) {}
};

template <typename Increaser = RealIncreaser>
struct AvgFloatItem {
  double sum;
  int64_t count;
  double min;
  double max;
  inline void record(double a) {
    Increaser::inc(count);
    Increaser::add(sum, a);
    Increaser::set(min, std::min(min, a));
    Increaser::set(max, std::max(max, a));
  }
  [[nodiscard]] inline double avg() const { return count != 0 ? sum / static_cast<double>(count) : 0; }
  inline void add_to_json(const std::string& name, nlohmann::json& json) const {
    json[name] = nullptr;
    json[name]["sum"] = sum;
    json[name]["count"] = count;
    json[name]["min"] = min;
    json[name]["max"] = max;
    json[name]["avg"] = avg();
  }
  inline void reset() {
    this->sum = 0;
    this->count = 0;
    this->min = std::numeric_limits<double>::max();
    this->max = std::numeric_limits<double>::min();
  }
  AvgFloatItem() : sum(0), count(0), min(std::numeric_limits<double>::max()), max(std::numeric_limits<double>::min()) {}

  AvgFloatItem& operator+=(const AvgFloatItem& rhs) {
    sum += rhs.sum;
    count += rhs.count;
    min = std::min(min, rhs.min);
    max = std::max(max, rhs.max);

    return *this;
  }
  friend AvgFloatItem operator+(AvgFloatItem lhs, const AvgFloatItem& rhs) {
    return lhs += rhs;
  }
};

template <class F, class Increaser = RealIncreaser>
inline static void update(const F& update_function) {
  Increaser::update(update_function);
}

}  // namespace statistics

#endif  // SRC_PROJECT_STATISTICS_HH
