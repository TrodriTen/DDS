// SPDX-License-Identifier: Apache-2.0
// Piezas comunes (no plantilla) del motor de publicador/suscriptor.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "bench/config.hpp"
#include "bench/histogram.hpp"
#include "bench/metrics.hpp"
#include "bench/proc_stats.hpp"
#include "bench/rt.hpp"

namespace bench {

// Bandera global de parada (SIGINT/SIGTERM).
std::atomic<bool>& stop_requested();
void install_signal_handlers();

// Preparación del proceso ANTES de crear entidades DDS:
//  - exporta DDS_BENCH_* para los XML (interfaz, IP, multicast, dominio)
//  - mlockall + prefault de heap, /dev/cpu_dma_latency
//  - hilo principal -> afinidad/prioridad del middleware: los hilos internos
//    que cree el middleware heredan SCHED_FIFO y la máscara de CPUs.
struct ProcessContext {
  rt::RtEnvironment env;
  std::vector<std::string> warnings;
  int dma_fd = -1;
  std::string output_prefix;  // <out_dir>/<run_id>/<role>_<middleware>_n<node>
};
ProcessContext prepare_process(BenchConfig& cfg);

// Hilo principal -> SCHED_OTHER en CPUs de housekeeping (tras crear entidades).
void enter_housekeeping(const BenchConfig& cfg);

void log_info(const BenchConfig& cfg, const std::string& msg);
void log_warn(const std::string& msg);

// Lee la etiqueta de fase (primera línea del fichero) o devuelve 'fallback'.
std::string read_phase(const std::string& path, const std::string& fallback);

// Métricas de host muestreadas por ventana por el hilo de estadísticas.
class HostSampler {
 public:
  explicit HostSampler(std::string iface);
  // Toma una muestra y añade al registro los deltas desde la anterior.
  void sample();
  void append(Record& r) const;
  [[nodiscard]] const ProcDelta& proc() const noexcept { return proc_delta_; }
  [[nodiscard]] double window_s() const noexcept { return window_s_; }

 private:
  std::string iface_;
  ProcSample proc_prev_{}, proc_cur_{};
  ProcDelta proc_delta_{};
  UdpSnmp udp_prev_{}, udp_cur_{};
  NicStats nic_prev_{}, nic_cur_{};
  double window_s_ = 0;
};

// Etiquetas comunes de cada fila (identifican el experimento).
void add_identity(Record& r, const BenchConfig& cfg, const char* stream, const std::string& phase);

// Tasa de incumplimiento de DEADLINE: fallos / periodos de deadline esperados.
double deadline_miss_rate(uint64_t missed, double elapsed_s, uint32_t instances, int64_t deadline_ns);

// Volcado de inventario de hilos (planificación efectiva de hilos del middleware).
void write_thread_report(const std::string& path, const BenchConfig& cfg);

// Metadatos del experimento (JSON plano).
void write_meta_json(const std::string& path, const BenchConfig& cfg, const ProcessContext& ctx,
                     const std::string& middleware_version);

// Rellena las rutas de configuración no indicadas por CLI buscando en:
// $DDS_BENCH_CONFIG_DIR, <exe>/../share/dds_bench/config, <exe>/../config,
// <exe>/../../config y ./config. Cyclone: base cyclonedds.xml + overlay
// por transporte (cyclonedds_multicast.xml / cyclonedds_shm.xml).
void resolve_default_configs(BenchConfig& cfg);

// Capacidad automática del almacén de muestras crudas.
size_t auto_raw_capacity(const BenchConfig& cfg, const StreamSpec& s, uint32_t divisor = 1);

// Cálculo de MB/s (10^6 bytes) y msg/s.
inline double rate(double count, double seconds) { return seconds > 0 ? count / seconds : 0.0; }

}  // namespace bench
