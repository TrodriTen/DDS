// SPDX-License-Identifier: Apache-2.0
// Detección de pérdidas, duplicados y desorden a partir del número de secuencia
// de aplicación (BenchHeader::seq), O(1) amortizado y sin asignaciones.
//
// Con varias instancias (keys), un take() puede entregar las muestras agrupadas
// por instancia y no en orden global; por eso se mantiene una ventana de
// reordenación (bitmap circular de kWindow secuencias) en lugar de exigir
// orden estricto. La pérdida se calcula de forma inmediata como:
//
//     lost = (max_seq_visto - primer_seq + 1) - muestras_únicas_recibidas
//
// y se corrige automáticamente si una muestra "perdida" llega tarde.
// Las muestras no recibidas tras la última vista (pérdida de cola) se
// contabilizan al cruzar los contadores de publicador y suscriptor.
#pragma once

#include <array>
#include <cstdint>

namespace bench {

class SequenceTracker {
 public:
  static constexpr uint64_t kWindow = 4096;  // múltiplo de 64

  void on_sample(uint64_t seq) noexcept {
    if (!started_) {
      started_ = true;
      first_ = max_seen_ = seq;
      bits_.fill(0);
      mark(seq);
      unique_ = 1;
      return;
    }
    if (seq > max_seen_) {
      const uint64_t advance = seq - max_seen_;
      if (advance >= kWindow) {
        bits_.fill(0);
      } else {
        for (uint64_t s = max_seen_ + 1; s <= seq; ++s) clear(s);
      }
      max_seen_ = seq;
      mark(seq);
      ++unique_;
      return;
    }
    if (seq < first_) {  // anterior al inicio de la medida: se ignora
      ++before_start_;
      return;
    }
    // seq <= max_seen_: llegada desordenada (o duplicada)
    if (max_seen_ - seq >= kWindow) {  // fuera de la ventana: no verificable
      ++late_;
      ++unique_;
      ++out_of_order_;
      return;
    }
    if (test(seq)) {
      ++duplicates_;
      return;
    }
    mark(seq);
    ++unique_;
    ++out_of_order_;
  }

  [[nodiscard]] bool started() const noexcept { return started_; }
  [[nodiscard]] uint64_t first() const noexcept { return first_; }
  [[nodiscard]] uint64_t max_seen() const noexcept { return max_seen_; }
  [[nodiscard]] uint64_t unique() const noexcept { return unique_; }
  [[nodiscard]] uint64_t expected() const noexcept { return started_ ? (max_seen_ - first_ + 1) : 0; }
  [[nodiscard]] uint64_t lost() const noexcept {
    const uint64_t e = expected();
    return e > unique_ ? e - unique_ : 0;
  }
  [[nodiscard]] uint64_t duplicates() const noexcept { return duplicates_; }
  [[nodiscard]] uint64_t out_of_order() const noexcept { return out_of_order_; }
  [[nodiscard]] uint64_t late() const noexcept { return late_; }

 private:
  void mark(uint64_t s) noexcept { bits_[(s % kWindow) >> 6] |= (uint64_t{1} << (s & 63)); }
  void clear(uint64_t s) noexcept { bits_[(s % kWindow) >> 6] &= ~(uint64_t{1} << (s & 63)); }
  [[nodiscard]] bool test(uint64_t s) const noexcept {
    return (bits_[(s % kWindow) >> 6] >> (s & 63)) & 1u;
  }

  std::array<uint64_t, kWindow / 64> bits_{};
  bool started_ = false;
  uint64_t first_ = 0;
  uint64_t max_seen_ = 0;
  uint64_t unique_ = 0;
  uint64_t duplicates_ = 0;
  uint64_t out_of_order_ = 0;
  uint64_t late_ = 0;
  uint64_t before_start_ = 0;
};

}  // namespace bench
