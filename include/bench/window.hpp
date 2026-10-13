// SPDX-License-Identifier: Apache-2.0
// Acumuladores de ventana con doble búfer, sin locks ni asignaciones.
//
// Los hilos RT sólo REGISTRAN en el búfer activo. El hilo de estadísticas
// (no-RT) rota el búfer al final de cada ventana, espera a que termine un
// registro en curso (decenas de ns) y calcula los percentiles fuera del
// camino crítico. Protocolo tipo Dekker con orden secuencialmente consistente:
//
//   productor:  i = active; busy[i] = 1; if (active != i) {busy[i]=0; retry}
//               ...registrar...; busy[i] = 0
//   colector :  active = 1 - i; while (busy[i]) spin; leer y resetear acc[i]
#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "bench/histogram.hpp"

namespace bench {

template <class Acc>
class DoubleBuffered {
 public:
  template <class F>
  void record(F&& f) noexcept {
    for (;;) {
      const int i = active_.load(std::memory_order_seq_cst);
      busy_[i].store(1, std::memory_order_seq_cst);
      if (active_.load(std::memory_order_seq_cst) != i) {
        busy_[i].store(0, std::memory_order_release);
        continue;
      }
      f(acc_[i]);
      busy_[i].store(0, std::memory_order_release);
      return;
    }
  }

  // Sólo desde el hilo colector. 'f' recibe el acumulador cerrado; después se resetea.
  template <class F>
  void collect(F&& f) {
    const int old = active_.load(std::memory_order_seq_cst);
    active_.store(1 - old, std::memory_order_seq_cst);
    while (busy_[old].load(std::memory_order_seq_cst) != 0) {
    }
    f(acc_[old]);
    acc_[old].reset();
  }

 private:
  std::array<Acc, 2> acc_{};
  std::array<std::atomic<int>, 2> busy_{};
  std::atomic<int> active_{0};
};

// ---- Acumuladores por tipo de hilo ----------------------------------------

// Suscriptor: hilo lector RT
struct SubWindowAcc {
  uint64_t samples = 0;
  uint64_t bytes = 0;
  uint64_t warmup_samples = 0;
  uint64_t echo_sent = 0;
  uint64_t echo_failed = 0;
  LatencyHistogram e2e;       // T2 - T1 (depende del offset NTP)
  LatencyHistogram e2e_corr;  // T2 - T1 - θ (offset estimado vía eco)
  // Instantánea de contadores acumulados del SequenceTracker
  bool seq_valid = false;
  uint64_t seq_unique = 0, seq_lost = 0, seq_dup = 0, seq_ooo = 0;

  void reset() noexcept {
    samples = bytes = warmup_samples = echo_sent = echo_failed = 0;
    e2e.reset();
    e2e_corr.reset();
    seq_valid = false;
  }
};

// Publicador: hilo escritor RT
struct PubWriteAcc {
  uint64_t samples = 0;
  uint64_t bytes = 0;
  uint64_t write_errors = 0;
  uint64_t loan_fallbacks = 0;
  uint64_t overruns = 0;           // ranuras de publicación saltadas por retraso
  LatencyHistogram write_call;     // duración de DataWriter::write()
  LatencyHistogram wakeup_latency; // despertar real - instante planificado

  void reset() noexcept {
    samples = bytes = write_errors = loan_fallbacks = overruns = 0;
    write_call.reset();
    wakeup_latency.reset();
  }
};

// Publicador: hilo de eco RT
struct PubEchoAcc {
  uint64_t echoes = 0;
  LatencyHistogram rtt;       // T4' - T1' sobre CLOCK_MONOTONIC del publicador
  LatencyHistogram rtt_net;   // RTT - (T3 - T2): sin el tiempo de proceso del suscriptor
  int64_t offset_ns = 0;      // θ estimado (filtro de mínimo retardo)
  int64_t offset_delay_ns = 0;
  bool offset_valid = false;

  void reset() noexcept {
    echoes = 0;
    rtt.reset();
    rtt_net.reset();
    offset_valid = false;
  }
};

// ---- Contadores de estado DDS (escritos desde callbacks de listener) -------

struct WriterStatusCounters {
  std::atomic<uint64_t> offered_deadline_missed{0};
  std::atomic<uint64_t> liveliness_lost{0};
  std::atomic<uint64_t> offered_incompatible_qos{0};
  std::atomic<int32_t> matched{0};
};

struct ReaderStatusCounters {
  std::atomic<uint64_t> requested_deadline_missed{0};
  std::atomic<uint64_t> sample_lost{0};
  std::atomic<uint64_t> sample_rejected{0};
  std::atomic<uint64_t> liveliness_alive_changes{0};
  std::atomic<uint64_t> liveliness_not_alive_changes{0};
  std::atomic<int32_t> liveliness_alive{0};
  std::atomic<uint64_t> requested_incompatible_qos{0};
  std::atomic<int32_t> matched{0};
  std::atomic<int32_t> matched_peak{0};
};

}  // namespace bench
