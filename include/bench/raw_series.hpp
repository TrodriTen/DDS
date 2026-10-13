// SPDX-License-Identifier: Apache-2.0
// Almacén preasignado de muestras crudas de latencia para percentiles EXACTOS
// al final de la ejecución (p99.9 exige N >> 1000 muestras sin agregación).
// La memoria se reserva y se "toca" en el arranque: el push() en el hilo RT
// nunca provoca asignaciones ni page-faults (con mlockall activo).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "bench/histogram.hpp"

namespace bench {

class RawSeries {
 public:
  RawSeries() = default;
  explicit RawSeries(size_t capacity) { reserve(capacity); }

  void reserve(size_t capacity) {
    capacity_ = capacity;
    data_ = std::make_unique<int64_t[]>(capacity);
    std::memset(data_.get(), 0, capacity * sizeof(int64_t));  // prefault
    size_ = 0;
    dropped_ = 0;
  }

  void push(int64_t v) noexcept {
    if (size_ < capacity_) {
      data_[size_++] = v;
    } else {
      ++dropped_;
    }
  }

  [[nodiscard]] size_t size() const noexcept { return size_; }
  [[nodiscard]] uint64_t dropped() const noexcept { return dropped_; }
  [[nodiscard]] bool complete() const noexcept { return dropped_ == 0; }

  // Percentiles exactos (método "nearest-rank"). NO usar en el hilo RT.
  [[nodiscard]] LatencySummary summarize_exact() const {
    LatencySummary s;
    if (size_ == 0) return s;
    std::vector<int64_t> v(data_.get(), data_.get() + size_);
    std::sort(v.begin(), v.end());
    const auto rank = [&](double q) {
      const auto r = static_cast<size_t>(std::ceil(q * static_cast<double>(v.size())));
      return v[std::min(v.size() - 1, r == 0 ? 0 : r - 1)];
    };
    long double sum = 0;
    for (int64_t x : v) sum += x;
    const long double mean = sum / static_cast<long double>(v.size());
    long double m2 = 0;
    for (int64_t x : v) {
      const long double d = static_cast<long double>(x) - mean;
      m2 += d * d;
      if (x < 0) ++s.negative;
    }
    s.count = v.size();
    s.mean = static_cast<double>(mean);
    s.stddev = v.size() > 1 ? static_cast<double>(std::sqrt(m2 / static_cast<long double>(v.size() - 1))) : 0.0;
    s.min = v.front();
    s.p50 = rank(0.50);
    s.p90 = rank(0.90);
    s.p95 = rank(0.95);
    s.p99 = rank(0.99);
    s.p999 = rank(0.999);
    s.p9999 = rank(0.9999);
    s.max = v.back();
    return s;
  }

 private:
  std::unique_ptr<int64_t[]> data_;
  size_t capacity_ = 0;
  size_t size_ = 0;
  uint64_t dropped_ = 0;
};

}  // namespace bench
