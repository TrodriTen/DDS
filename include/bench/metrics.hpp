// SPDX-License-Identifier: Apache-2.0
// Exportación de métricas: CSV (serie temporal + resumen) y Prometheus
// (exposición HTTP /metrics y fichero para node_exporter textfile collector).
// Todo se ejecuta en el hilo de estadísticas (no-RT).
#pragma once

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "bench/histogram.hpp"

namespace bench {

// Registro ordenado clave -> valor (una fila CSV / un conjunto de métricas).
class Record {
 public:
  Record& add(std::string_view key, const std::string& v, bool label = false);
  Record& add(std::string_view key, const char* v, bool label = false) { return add(key, std::string(v), label); }
  Record& add(std::string_view key, double v, int precision = 3);
  Record& add(std::string_view key, int64_t v);
  Record& add(std::string_view key, uint64_t v);
  Record& add(std::string_view key, int v) { return add(key, static_cast<int64_t>(v)); }
  Record& add(std::string_view key, uint32_t v) { return add(key, static_cast<uint64_t>(v)); }
  Record& add_empty(std::string_view key);
  // Añade mean/std/min/p50/p90/p95/p99/p999/p9999/max en microsegundos con el prefijo dado.
  Record& add_latency_us(std::string_view prefix, const LatencySummary& s, bool full = true);

  struct Field {
    std::string key;
    std::string value;
    bool label = false;    // etiqueta Prometheus (no métrica)
    bool numeric = false;  // valor numérico exportable
  };
  [[nodiscard]] const std::vector<Field>& fields() const noexcept { return fields_; }

 private:
  std::vector<Field> fields_;
};

class CsvSink {
 public:
  bool open(const std::string& path);
  void write(const Record& r);
  [[nodiscard]] const std::string& path() const noexcept { return path_; }

 private:
  std::ofstream out_;
  std::string path_;
  bool header_written_ = false;
};

// Convierte registros en formato de exposición de texto de Prometheus.
// Los campos 'label' se usan como etiquetas; los numéricos como gauges
// "dds_bench_<clave>".
std::string to_prometheus(const std::vector<Record>& records, std::string_view prefix = "dds_bench_");

// Servidor HTTP mínimo (GET /metrics) en un hilo no-RT.
class PromHttpExporter {
 public:
  PromHttpExporter() = default;
  ~PromHttpExporter();
  PromHttpExporter(const PromHttpExporter&) = delete;
  PromHttpExporter& operator=(const PromHttpExporter&) = delete;

  bool start(int port, std::string* error);
  void stop();
  void set_text(std::string text);

 private:
  void serve();
  int listen_fd_ = -1;
  std::thread thread_;
  std::mutex mu_;
  std::string text_;
  bool running_ = false;
};

// Escritura atómica (tmp + rename) para el textfile collector.
bool write_file_atomic(const std::string& path, const std::string& content);

}  // namespace bench
