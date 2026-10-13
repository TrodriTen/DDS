// SPDX-License-Identifier: Apache-2.0
// Configuración del benchmark: CLI, presets de QoS por sensor y especificación
// de QoS neutral respecto al middleware (cada backend la traduce a su API).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bench/rt.hpp"

namespace bench {

enum class SensorKind : uint8_t { Radar = 1, Sonar = 2, Weather = 3 };
enum class Reliability : uint8_t { Reliable, BestEffort };
enum class HistoryKind : uint8_t { KeepLast, KeepAll };
enum class LivelinessKind : uint8_t { Automatic, ManualByParticipant, ManualByTopic };
enum class DataRep : uint8_t { Default, Xcdr1, Xcdr2 };

[[nodiscard]] const char* to_string(SensorKind k) noexcept;
[[nodiscard]] const char* to_string(Reliability r) noexcept;
[[nodiscard]] const char* to_string(HistoryKind h) noexcept;
[[nodiscard]] const char* to_string(LivelinessKind l) noexcept;
[[nodiscard]] const char* to_string(DataRep d) noexcept;
[[nodiscard]] std::optional<SensorKind> parse_sensor(std::string_view s) noexcept;

// Especificación de QoS. Los campos vacíos (nullopt) no se aplican y conservan
// el valor base (perfil XML del middleware o valor por defecto del estándar).
struct QosSpec {
  std::optional<Reliability> reliability;
  std::optional<int64_t> max_blocking_ns;
  std::optional<HistoryKind> history;
  std::optional<int32_t> depth;
  // ResourceLimitsQosPolicy (-1 = LENGTH_UNLIMITED)
  std::optional<int32_t> max_samples;
  std::optional<int32_t> max_instances;
  std::optional<int32_t> max_samples_per_instance;
  std::optional<int64_t> deadline_ns;        // 0 = infinito
  std::optional<int64_t> latency_budget_ns;  // 0 = sin presupuesto
  std::optional<LivelinessKind> liveliness;
  std::optional<int64_t> lease_ns;           // 0 = infinito
  std::optional<int32_t> transport_priority;
  std::optional<DataRep> data_rep;

  // Sobrescribe los campos presentes en 'o'.
  void merge_from(const QosSpec& o);
  [[nodiscard]] std::string describe() const;
};

struct StreamSpec {
  SensorKind kind = SensorKind::Radar;
  std::string topic;          // nombre del topic de datos
  std::string echo_topic;     // topic de eco (RTT)
  double rate_hz = 100.0;     // frecuencia POR INSTANCIA
  uint32_t instances = 1;     // nº de claves (pistas, transductores, estaciones)
  uint32_t sonar_min_bytes = 256;
  uint32_t sonar_max_bytes = 1024;
  QosSpec qos;                // QoS efectiva (preset + CLI, o sólo CLI si hay perfil XML)
  std::string qos_profile;    // perfil XML (Fast DDS: nombre; Cyclone: lib::perfil)
  uint32_t stream_id = 0;     // (node_id << 8) | kind

  [[nodiscard]] double aggregate_rate_hz() const noexcept { return rate_hz * instances; }
  [[nodiscard]] int64_t period_ns() const noexcept;
};

// Preset por sensor (valores por defecto documentados en docs/QOS.md).
[[nodiscard]] StreamSpec default_stream(SensorKind k);

struct BenchConfig {
  std::string role;        // "pub" | "sub"
  std::string middleware;  // "fastdds" | "cyclonedds"
  std::string program;     // argv[0]

  // Identificación del experimento
  uint32_t node_id = 1;
  int domain_id = 42;
  std::string run_id;
  std::string scenario = "default";
  std::string transport = "udp";  // udp | udpm | shm | zc  (etiqueta y perfil Fast DDS)
  std::string topic_prefix = "bench_";

  std::vector<StreamSpec> streams;

  // Temporización
  double duration_s = 60.0;
  double warmup_s = 5.0;
  double match_timeout_s = 30.0;
  int expected_matches = 1;
  int64_t window_ns = 1'000'000'000;
  double drain_s = 2.0;

  // RTT / eco
  uint32_t echo_every = 1;   // pub: pide eco cada N muestras (0 = sin RTT)
  bool echo_enabled = true;  // sub: responde a peticiones de eco

  // Zero-copy (loans) cuando el tipo y el transporte lo permiten
  bool loans = false;

  // DSCP / SO_PRIORITY (-1 = derivar de TRANSPORT_PRIORITY, -2 = desactivado)
  int dscp = -1;
  int so_priority = -1;

  // Ficheros de configuración del middleware
  std::string fastdds_xml;           // perfiles Fast DDS
  std::string participant_profile;   // perfil de participante Fast DDS
  std::string fastdds_memory_policy; // preallocated | preallocated_realloc | dynamic | dynamic_reusable
  std::string cyclone_xml;           // configuración de dominio Cyclone (CYCLONEDDS_URI)
  std::string cyclone_xml_overlay;   // overlay por transporte (multicast / shm)
  std::string cyclone_qos_xml;       // QoS Provider OMG DDS-XML de Cyclone
  std::string iface;                 // interfaz de red de pruebas (DDS_BENCH_IFACE)
  std::string mcast_addr = "239.255.0.2";  // grupo de DATOS (SPDP usa 239.255.0.1)

  rt::RtOptions rt;

  // Salidas
  std::string out_dir = "results";
  int prom_port = 0;            // 0 = sin exportador HTTP
  std::string prom_textfile;    // node_exporter textfile collector
  std::string phase_file;       // etiqueta de fase (baseline/noise/recovery)
  size_t raw_capacity = 0;      // 0 = automático (tasa * duración * 1.2)
  uint64_t seed = 42;
  bool verbose = false;
  bool quiet = false;

  [[nodiscard]] std::string describe() const;
};

// Parsea argv. Devuelve false (y rellena 'error') ante opciones inválidas.
// Si se pide --help, imprime la ayuda y termina el proceso.
bool parse_cli(int argc, char** argv, const std::string& role, const std::string& middleware,
               BenchConfig& cfg, std::string& error);

}  // namespace bench
