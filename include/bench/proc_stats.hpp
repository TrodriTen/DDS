// SPDX-License-Identifier: Apache-2.0
// Métricas de proceso y de red leídas de /proc y /sys (hilo no-RT).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bench {

struct ProcSample {
  int64_t t_mono_ns = 0;
  uint64_t utime_ticks = 0;
  uint64_t stime_ticks = 0;
  uint64_t rss_kb = 0;
  uint64_t hwm_kb = 0;  // pico de RSS
  uint64_t vol_ctxsw = 0;
  uint64_t invol_ctxsw = 0;
  uint64_t minflt = 0;
  uint64_t majflt = 0;
  uint32_t threads = 0;
};

// Contadores UDP del host (/proc/net/snmp; espacio de red completo, no por proceso).
struct UdpSnmp {
  uint64_t in_datagrams = 0, no_ports = 0, in_errors = 0, out_datagrams = 0;
  uint64_t rcvbuf_errors = 0, sndbuf_errors = 0, in_csum_errors = 0;
};

// Estadísticas de la interfaz de red (/sys/class/net/<if>/statistics).
struct NicStats {
  uint64_t rx_packets = 0, tx_packets = 0, rx_bytes = 0, tx_bytes = 0;
  uint64_t rx_dropped = 0, tx_dropped = 0, rx_errors = 0, tx_errors = 0;
};

struct ProcDelta {
  double cpu_pct = 0;  // % de UN núcleo (puede superar 100 con varios hilos)
  double cpu_user_pct = 0;
  double cpu_sys_pct = 0;
  uint64_t rss_kb = 0;
  uint64_t hwm_kb = 0;
  uint64_t vol_ctxsw = 0, invol_ctxsw = 0;
  uint64_t minflt = 0, majflt = 0;
  uint32_t threads = 0;
};

[[nodiscard]] ProcSample read_proc_sample();
[[nodiscard]] ProcDelta diff(const ProcSample& prev, const ProcSample& cur);
[[nodiscard]] UdpSnmp read_udp_snmp();
[[nodiscard]] NicStats read_nic_stats(const std::string& iface);

// Interfaz de la ruta por defecto (fallback "lo").
[[nodiscard]] std::string default_route_interface();
// IPv4 de una interfaz (vacío si no existe).
[[nodiscard]] std::string interface_ipv4(const std::string& iface);

struct ThreadInfo {
  int tid = 0;
  std::string name;
  std::string sched;  // "FIFO:70 cpus=2,3"
  double cpu_s = 0;
};
// Inventario de hilos del proceso (incluye los internos del middleware).
[[nodiscard]] std::vector<ThreadInfo> list_threads();

}  // namespace bench
