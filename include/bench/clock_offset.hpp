// SPDX-License-Identifier: Apache-2.0
// Estimación del desfase (offset) de reloj publicador <-> suscriptor a partir
// del intercambio de eco, con el mismo modelo que NTP (RFC 5905 §8):
//
//      T1 = pub tx (reloj pub)      T2 = sub rx (reloj sub)
//      T4 = pub rx (reloj pub)      T3 = sub tx (reloj sub)
//
//      δ (retardo de ida+vuelta neto) = (T4 - T1) - (T3 - T2)
//      θ (offset sub - pub)          = ((T2 - T1) + (T3 - T4)) / 2
//
// El error de θ está acotado por ±δ/2 y es nulo si los caminos son simétricos.
// Igual que el "clock filter" de NTP, se elige θ de la muestra con MENOR δ en
// una ventana deslizante: las muestras retrasadas por colas (p.ej. durante la
// inyección de ruido) tienen δ grande y asimetría alta, y se descartan.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>

namespace bench {

struct OffsetSample {
  int64_t delay_ns = std::numeric_limits<int64_t>::max();  // δ
  int64_t offset_ns = 0;                                   // θ
};

class ClockOffsetEstimator {
 public:
  static constexpr size_t kWindow = 64;

  // t1/t4: CLOCK_REALTIME del publicador; t2/t3: CLOCK_REALTIME del suscriptor.
  // Devuelve la muestra calculada para esta respuesta.
  OffsetSample add(int64_t t1, int64_t t2, int64_t t3, int64_t t4) noexcept {
    OffsetSample s;
    s.delay_ns = (t4 - t1) - (t3 - t2);
    s.offset_ns = ((t2 - t1) + (t3 - t4)) / 2;
    window_[next_++ % kWindow] = s;
    if (count_ < kWindow) ++count_;
    // Selección de mínimo retardo (O(64), fuera del camino del writer).
    OffsetSample best;
    for (size_t i = 0; i < count_; ++i) {
      if (window_[i].delay_ns >= 0 && window_[i].delay_ns < best.delay_ns) best = window_[i];
    }
    if (best.delay_ns != std::numeric_limits<int64_t>::max()) {
      best_offset_.store(best.offset_ns, std::memory_order_relaxed);
      best_delay_.store(best.delay_ns, std::memory_order_relaxed);
      valid_.store(true, std::memory_order_release);
    }
    return s;
  }

  // Lectura lock-free desde el hilo writer (para incrustarla en BenchHeader).
  [[nodiscard]] bool valid() const noexcept { return valid_.load(std::memory_order_acquire); }
  [[nodiscard]] int64_t offset_ns() const noexcept { return best_offset_.load(std::memory_order_relaxed); }
  [[nodiscard]] int64_t delay_ns() const noexcept { return best_delay_.load(std::memory_order_relaxed); }

 private:
  std::array<OffsetSample, kWindow> window_{};
  size_t next_ = 0;
  size_t count_ = 0;
  std::atomic<int64_t> best_offset_{0};
  std::atomic<int64_t> best_delay_{0};
  std::atomic<bool> valid_{false};
};

}  // namespace bench
