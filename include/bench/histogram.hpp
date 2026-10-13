// SPDX-License-Identifier: Apache-2.0
// Histograma log-lineal (estilo HdrHistogram) de tamaño fijo, sin asignaciones
// dinámicas: apto para registrar en el hilo de tiempo real.
//
//  - 128 sub-buckets por octava  -> error relativo máximo < 0.8 %
//  - Rango [0, 2^38) ns (~275 s); los valores mayores se saturan.
//  - Media y desviación estándar EXACTAS (Welford), no derivadas del histograma.
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace bench {

class LatencyHistogram {
 public:
  static constexpr int kSubBits = 7;
  static constexpr int64_t kSub = int64_t{1} << kSubBits;
  static constexpr int kMaxBits = 38;
  static constexpr int kBuckets = (kMaxBits - kSubBits + 1) * static_cast<int>(kSub);
  static constexpr int64_t kMaxValue = (int64_t{1} << kMaxBits) - 1;

  void record(int64_t v) noexcept {
    // E2E negativa => offset de reloj mayor que la latencia real. Se conserva
    // el valor en min/media/desviación; para el bucket se satura a 0.
    if (v < 0) ++negative_;
    const int64_t c = std::clamp<int64_t>(v, 0, kMaxValue);
    ++counts_[static_cast<size_t>(index_of(c))];
    ++total_;
    min_ = std::min(min_, v);
    max_ = std::max(max_, v);
    // Welford (estable numéricamente)
    const double x = static_cast<double>(v);
    const double delta = x - mean_;
    mean_ += delta / static_cast<double>(total_);
    m2_ += delta * (x - mean_);
  }

  void reset() noexcept {
    counts_.fill(0);
    total_ = negative_ = 0;
    min_ = std::numeric_limits<int64_t>::max();
    max_ = std::numeric_limits<int64_t>::min();
    mean_ = m2_ = 0.0;
  }

  void merge(const LatencyHistogram& o) noexcept {
    if (o.total_ == 0) return;
    for (int i = 0; i < kBuckets; ++i) counts_[static_cast<size_t>(i)] += o.counts_[static_cast<size_t>(i)];
    // Combinación de Welford en paralelo (Chan et al.)
    const double n_a = static_cast<double>(total_);
    const double n_b = static_cast<double>(o.total_);
    const double delta = o.mean_ - mean_;
    const double n = n_a + n_b;
    mean_ += delta * n_b / n;
    m2_ += o.m2_ + delta * delta * n_a * n_b / n;
    total_ += o.total_;
    negative_ += o.negative_;
    min_ = std::min(min_, o.min_);
    max_ = std::max(max_, o.max_);
  }

  [[nodiscard]] uint64_t count() const noexcept { return total_; }
  [[nodiscard]] uint64_t negative_count() const noexcept { return negative_; }
  [[nodiscard]] int64_t min() const noexcept { return total_ ? min_ : 0; }
  [[nodiscard]] int64_t max() const noexcept { return total_ ? max_ : 0; }
  [[nodiscard]] double mean() const noexcept { return total_ ? mean_ : 0.0; }
  [[nodiscard]] double stddev() const noexcept {
    return total_ > 1 ? std::sqrt(m2_ / static_cast<double>(total_ - 1)) : 0.0;
  }

  // Percentil q en [0,1]. Devuelve el límite superior del bucket que contiene
  // el rango ceil(q*N) (conservador), acotado por el máximo observado.
  [[nodiscard]] int64_t percentile(double q) const noexcept {
    if (total_ == 0) return 0;
    if (q <= 0.0) return min();
    const auto rank = static_cast<uint64_t>(std::ceil(q * static_cast<double>(total_)));
    uint64_t acc = 0;
    for (int i = 0; i < kBuckets; ++i) {
      acc += counts_[static_cast<size_t>(i)];
      if (acc >= rank) return std::clamp(upper_bound_of(i), min(), max());
    }
    return max();
  }

  // Índice de bucket; público para tests.
  [[nodiscard]] static constexpr int index_of(int64_t v) noexcept {
    if (v < kSub) return static_cast<int>(v);
    const int msb = 63 - std::countl_zero(static_cast<uint64_t>(v));
    const int shift = msb - kSubBits;
    return (shift + 1) * static_cast<int>(kSub) + static_cast<int>((v >> shift) - kSub);
  }

  [[nodiscard]] static constexpr int64_t upper_bound_of(int idx) noexcept {
    const int group = idx / static_cast<int>(kSub);
    const int64_t sub = idx % kSub;
    if (group == 0) return sub;
    const int shift = group - 1;
    return ((kSub + sub) << shift) + ((int64_t{1} << shift) - 1);
  }

 private:
  std::array<uint64_t, kBuckets> counts_{};
  uint64_t total_ = 0;
  uint64_t negative_ = 0;
  int64_t min_ = std::numeric_limits<int64_t>::max();
  int64_t max_ = std::numeric_limits<int64_t>::min();
  double mean_ = 0.0;
  double m2_ = 0.0;
};

// Resumen estadístico serializable (ns).
struct LatencySummary {
  uint64_t count = 0;
  double mean = 0, stddev = 0;
  int64_t min = 0, p50 = 0, p90 = 0, p95 = 0, p99 = 0, p999 = 0, p9999 = 0, max = 0;
  uint64_t negative = 0;
};

[[nodiscard]] inline LatencySummary summarize(const LatencyHistogram& h) noexcept {
  LatencySummary s;
  s.count = h.count();
  s.mean = h.mean();
  s.stddev = h.stddev();
  s.min = h.min();
  s.p50 = h.percentile(0.50);
  s.p90 = h.percentile(0.90);
  s.p95 = h.percentile(0.95);
  s.p99 = h.percentile(0.99);
  s.p999 = h.percentile(0.999);
  s.p9999 = h.percentile(0.9999);
  s.max = h.max();
  s.negative = h.negative_count();
  return s;
}

}  // namespace bench
