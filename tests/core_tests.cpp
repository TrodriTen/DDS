// SPDX-License-Identifier: Apache-2.0
// Tests unitarios del núcleo del arnés (sin dependencias de DDS).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

#include "bench/clock_offset.hpp"
#include "bench/config.hpp"
#include "bench/dscp.hpp"
#include "bench/histogram.hpp"
#include "bench/raw_series.hpp"
#include "bench/rt.hpp"
#include "bench/seq_tracker.hpp"
#include "bench/window.hpp"

namespace {
int g_failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FALLO %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

void test_histogram_indexing() {
  using H = bench::LatencyHistogram;
  int prev = -1;
  for (int64_t v = 0; v < (int64_t{1} << 20); v += 7) {
    const int idx = H::index_of(v);
    CHECK(idx >= prev);  // monótono
    CHECK(H::upper_bound_of(idx) >= v);
    prev = idx;
  }
  CHECK(H::index_of(H::kMaxValue) < H::kBuckets);
  // error relativo del límite superior < 1/128
  for (int64_t v : {1000LL, 123456LL, 9876543LL, 2000000000LL}) {
    const double ub = static_cast<double>(H::upper_bound_of(H::index_of(v)));
    CHECK((ub - static_cast<double>(v)) / static_cast<double>(v) < 1.0 / 128 + 1e-9);
  }
}

void test_histogram_vs_exact() {
  std::mt19937_64 rng(7);
  std::lognormal_distribution<double> dist(std::log(150'000.0), 0.6);  // ~150 µs
  bench::LatencyHistogram h;
  bench::RawSeries raw(200'000);
  for (int i = 0; i < 200'000; ++i) {
    const auto v = static_cast<int64_t>(dist(rng));
    h.record(v);
    raw.push(v);
  }
  const auto approx = bench::summarize(h);
  const auto exact = raw.summarize_exact();
  CHECK(exact.count == 200'000);
  const auto rel = [](int64_t a, int64_t b) { return std::abs(static_cast<double>(a - b)) / static_cast<double>(b); };
  CHECK(rel(approx.p50, exact.p50) < 0.01);
  CHECK(rel(approx.p99, exact.p99) < 0.01);
  CHECK(rel(approx.p999, exact.p999) < 0.01);
  CHECK(std::abs(approx.mean - exact.mean) / exact.mean < 1e-9);  // Welford exacto
  CHECK(std::abs(approx.stddev - exact.stddev) / exact.stddev < 1e-6);
  CHECK(approx.max == exact.max && approx.min == exact.min);

  // merge
  bench::LatencyHistogram a, b;
  for (int i = 1; i <= 1000; ++i) (i % 2 ? a : b).record(i * 1000);
  a.merge(b);
  CHECK(a.count() == 1000);
  CHECK(std::abs(a.mean() - 500500.0) < 1e-6);
  // negativos (offset de reloj)
  bench::LatencyHistogram n;
  n.record(-5);
  CHECK(n.negative_count() == 1 && n.min() == -5);
}

void test_seq_tracker() {
  bench::SequenceTracker t;
  for (uint64_t s = 100; s < 200; ++s) t.on_sample(s);
  CHECK(t.lost() == 0 && t.unique() == 100);
  // hueco de 5
  for (uint64_t s = 205; s < 210; ++s) t.on_sample(s);
  CHECK(t.lost() == 5);
  // llegan tarde 2 de los perdidos (reordenación entre instancias)
  t.on_sample(201);
  t.on_sample(203);
  CHECK(t.lost() == 3 && t.out_of_order() == 2);
  // duplicado
  t.on_sample(203);
  CHECK(t.duplicates() == 1 && t.lost() == 3);
  // salto mayor que la ventana
  t.on_sample(210 + bench::SequenceTracker::kWindow * 3);
  CHECK(t.lost() == 3 + bench::SequenceTracker::kWindow * 3);
  // anterior al inicio: ignorado
  t.on_sample(50);
  CHECK(t.unique() == 108);
}

void test_clock_offset() {
  bench::ClockOffsetEstimator est;
  // reloj sub adelantado 2 ms; ida 100 µs, vuelta 100 µs, proceso sub 20 µs
  const int64_t theta = 2'000'000;
  for (int i = 0; i < 100; ++i) {
    const int64_t t1 = 1'000'000'000LL + i * 1'000'000LL;
    const int64_t extra = (i % 10 == 0) ? 900'000 : 0;  // muestras con colas (asimétricas)
    const int64_t t2 = t1 + 100'000 + extra + theta;
    const int64_t t3 = t2 + 20'000;
    const int64_t t4 = t3 - theta + 100'000;
    est.add(t1, t2, t3, t4);
  }
  CHECK(est.valid());
  CHECK(std::llabs(est.offset_ns() - theta) < 1000);
  CHECK(std::llabs(est.delay_ns() - 200'000) < 1000);
}

void test_dscp() {
  CHECK(bench::parse_dscp("EF") == 46);
  CHECK(bench::parse_dscp("af41") == 34);
  CHECK(bench::parse_dscp("CS5") == 40);
  CHECK(bench::parse_dscp("auto") == -1);
  CHECK(bench::parse_dscp("off") == -2);
  CHECK(bench::parse_dscp("26") == 26);
  CHECK(bench::map_transport_priority(50).dscp == 46);
  CHECK(bench::map_transport_priority(30).dscp == 34);
  CHECK(bench::map_transport_priority(0).dscp == 0);
}

void test_cpu_list() {
  const auto v = bench::rt::parse_cpu_list("0,2-4,7");
  CHECK((v == std::vector<int>{0, 2, 3, 4, 7}));
}

void test_double_buffer_concurrent() {
  struct Acc {
    uint64_t n = 0;
    void reset() { n = 0; }
  };
  bench::DoubleBuffered<Acc> db;
  std::atomic<bool> stop{false};
  constexpr uint64_t kTotal = 2'000'000;
  std::thread producer([&] {
    for (uint64_t i = 0; i < kTotal; ++i) db.record([](Acc& a) { ++a.n; });
    stop = true;
  });
  uint64_t collected = 0;
  while (!stop) db.collect([&](const Acc& a) { collected += a.n; });
  producer.join();
  db.collect([&](const Acc& a) { collected += a.n; });
  db.collect([&](const Acc& a) { collected += a.n; });
  CHECK(collected == kTotal);  // ni pérdidas ni dobles conteos
}

void test_cli() {
  const char* argv[] = {"bench", "--streams", "radar,sonar", "--reliability", "best_effort", "--deadline-ms",
                        "radar=10", "--rate", "radar=50", "--instances", "radar=100", "--duration", "5"};
  bench::BenchConfig cfg;
  std::string err;
  CHECK(bench::parse_cli(static_cast<int>(std::size(argv)), const_cast<char**>(argv), "pub", "fastdds", cfg, err));
  CHECK(cfg.streams.size() == 2);
  const auto& r = cfg.streams[0];
  CHECK(r.kind == bench::SensorKind::Radar);
  CHECK(*r.qos.reliability == bench::Reliability::BestEffort);
  CHECK(*r.qos.deadline_ns == 10'000'000);
  CHECK(r.rate_hz == 50.0 && r.instances == 100);
  CHECK(*r.qos.max_instances >= 100);  // ResourceLimits ajustado al nº de instancias
  CHECK(*cfg.streams[1].qos.deadline_ns == 50'000'000);  // sonar conserva su preset
  CHECK(cfg.streams[1].qos.reliability == bench::Reliability::BestEffort);
}
}  // namespace

int main() {
  test_histogram_indexing();
  test_histogram_vs_exact();
  test_seq_tracker();
  test_clock_offset();
  test_dscp();
  test_cpu_list();
  test_double_buffer_concurrent();
  test_cli();
  if (g_failures == 0) std::printf("core_tests: OK\n");
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
