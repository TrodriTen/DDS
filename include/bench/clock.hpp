// SPDX-License-Identifier: Apache-2.0
// Relojes y espera periódica determinista.
//
//  - CLOCK_REALTIME  : reloj de pared sincronizado (NTP/PTP). Se usa SOLO para
//                      la latencia End-to-End entre nodos (T1 pub -> T2 sub).
//  - CLOCK_MONOTONIC : reloj local sin saltos. Se usa para RTT, planificación
//                      de publicación y ventanas de métricas.
#pragma once

#include <cerrno>
#include <cstdint>
#include <ctime>

namespace bench {

inline constexpr int64_t kNsPerUs = 1'000;
inline constexpr int64_t kNsPerMs = 1'000'000;
inline constexpr int64_t kNsPerSec = 1'000'000'000;

[[nodiscard]] inline int64_t to_ns(const timespec& ts) noexcept {
  return static_cast<int64_t>(ts.tv_sec) * kNsPerSec + ts.tv_nsec;
}

[[nodiscard]] inline timespec to_timespec(int64_t ns) noexcept {
  timespec ts{};
  ts.tv_sec = static_cast<time_t>(ns / kNsPerSec);
  ts.tv_nsec = static_cast<long>(ns % kNsPerSec);
  return ts;
}

[[nodiscard]] inline int64_t now_ns(clockid_t clk) noexcept {
  timespec ts{};
  clock_gettime(clk, &ts);
  return to_ns(ts);
}

[[nodiscard]] inline int64_t now_rt_ns() noexcept { return now_ns(CLOCK_REALTIME); }
[[nodiscard]] inline int64_t now_mono_ns() noexcept { return now_ns(CLOCK_MONOTONIC); }

// Duerme hasta un instante absoluto de CLOCK_MONOTONIC (sin deriva acumulada,
// a diferencia de sleep relativo). Reintenta ante EINTR.
inline void sleep_until_mono_ns(int64_t abs_ns) noexcept {
  const timespec ts = to_timespec(abs_ns);
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
  }
}

}  // namespace bench
