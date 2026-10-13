// SPDX-License-Identifier: Apache-2.0
// Utilidades de tiempo real para Linux PREEMPT_RT.
#pragma once

#include <pthread.h>

#include <cstdint>
#include <string>
#include <vector>

namespace bench::rt {

struct RtOptions {
  bool enable_fifo = true;          // SCHED_FIFO para hilos de aplicación
  int app_priority = 80;            // prioridad hilo writer/reader
  int echo_priority = 79;           // prioridad del hilo de eco/RTT
  int middleware_priority = 70;     // heredada por hilos internos del middleware
  std::vector<int> app_cpus;        // afinidad hilos RT de aplicación (vacío = sin cambio)
  std::vector<int> middleware_cpus; // afinidad heredada por hilos del middleware
  std::vector<int> housekeeping_cpus; // hilo de métricas/exportación (SCHED_OTHER)
  bool lock_memory = true;          // mlockall(MCL_CURRENT | MCL_FUTURE)
  size_t prefault_stack_kb = 512;   // pila pre-tocada por hilo RT
  size_t prefault_heap_mb = 64;     // heap pre-tocado (con M_TRIM deshabilitado)
  int cpu_dma_latency_us = -1;      // >=0: mantiene /dev/cpu_dma_latency abierto
};

// Estado del entorno (para el informe y validación previa).
struct RtEnvironment {
  std::string kernel_release;
  std::string kernel_version;
  bool preempt_rt = false;          // /sys/kernel/realtime == 1 o "PREEMPT_RT" en uname -v
  std::string isolated_cpus;        // /sys/devices/system/cpu/isolated
  std::string nohz_full;            // /sys/devices/system/cpu/nohz_full
  long sched_rt_runtime_us = 0;     // -1 = throttling RT deshabilitado
  std::string cpu_governor;         // cpu0
  std::string clocksource;
  uint64_t rlimit_rtprio = 0;
  uint64_t rlimit_memlock = 0;      // bytes (UINT64_MAX = ilimitado)
  // Estado NTP del reloj (adjtimex)
  bool ntp_synchronized = false;
  long ntp_esterror_us = -1;        // error estimado por el kernel
  long ntp_maxerror_us = -1;
  long ntp_offset_us = 0;
};

[[nodiscard]] RtEnvironment probe_environment();
std::string describe(const RtEnvironment& env);

// Bloquea memoria y desactiva la devolución de memoria al SO (malloc trim/mmap)
// para que el heap pre-tocado se quede residente. Devuelve false si mlockall falla.
bool lock_process_memory(const RtOptions& opts, std::string* error);

// Pre-toca 'kb' KiB de pila del hilo llamante (evita page-faults en la pila).
void prefault_stack(size_t kb);

// Aplica afinidad y política al hilo indicado. priority<=0 => SCHED_OTHER.
bool configure_thread(pthread_t thread, const std::vector<int>& cpus, int priority,
                      std::string* error);

// Configura el hilo llamante (afinidad + SCHED_FIFO) y pre-toca su pila.
bool configure_current_thread(const char* name, const std::vector<int>& cpus, int priority,
                              size_t prefault_stack_kb, std::string* error);

// Mantiene abierto /dev/cpu_dma_latency con el valor dado (evita C-states
// profundos). Devuelve fd o -1. Se libera al terminar el proceso.
int hold_cpu_dma_latency(int32_t microseconds);

// Parsing de listas de CPUs: "2,3", "2-5", "0,2-3".
std::vector<int> parse_cpu_list(const std::string& s);
std::string format_cpu_list(const std::vector<int>& cpus);

// Política/prioridad efectiva del hilo (para el informe de hilos).
std::string thread_sched_string(int tid);

}  // namespace bench::rt
