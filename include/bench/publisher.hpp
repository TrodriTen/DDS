// SPDX-License-Identifier: Apache-2.0
// Motor genérico del PUBLICADOR (Data Writer), plantilla sobre el backend DDS.
//
// Hilos:
//   bench.writer (SCHED_FIFO app_priority)  : planificador periódico de todos los
//       flujos con instantes absolutos (clock_nanosleep TIMER_ABSTIME), generación
//       sintética, sellado T1 y write().
//   bench.echo   (SCHED_FIFO echo_priority) : WaitSet sobre los topics de eco ->
//       RTT en reloj MONOTONIC local + estimación de offset tipo NTP.
//   main/bench.stats (SCHED_OTHER)           : ventanas de métricas, CSV, Prometheus.
//
// Requisitos del TU que lo incluye: haber incluido antes la cabecera generada
// de SensorsTelemetry.idl y la del backend (FastDdsBackend / CycloneBackend).
#pragma once

#include <algorithm>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "bench/clock.hpp"
#include "bench/clock_offset.hpp"
#include "bench/config.hpp"
#include "bench/dscp.hpp"
#include "bench/harness_common.hpp"
#include "bench/histogram.hpp"
#include "bench/metrics.hpp"
#include "bench/raw_series.hpp"
#include "bench/synthetic.hpp"
#include "bench/window.hpp"

namespace bench {

// QoS del canal de eco: misma fiabilidad y prioridad que el flujo, sin deadline.
inline QosSpec make_echo_qos(const StreamSpec& s) {
  QosSpec e;
  e.reliability = s.qos.reliability.value_or(Reliability::Reliable);
  e.max_blocking_ns = 100 * kNsPerMs;
  e.history = HistoryKind::KeepLast;
  e.depth = 64;
  e.max_samples = -1;
  e.max_instances = -1;
  e.max_samples_per_instance = -1;
  e.deadline_ns = 0;
  e.latency_budget_ns = 0;
  e.liveliness = LivelinessKind::Automatic;
  e.lease_ns = 0;
  e.transport_priority = s.qos.transport_priority.value_or(0);
  e.data_rep = s.qos.data_rep.value_or(DataRep::Default);
  return e;
}

// ---------------------------------------------------------------------------
template <class B>
class PubStreamBase {
 public:
  explicit PubStreamBase(const StreamSpec& s, const BenchConfig& cfg)
      : spec_(s),
        raw_rtt_(cfg.echo_every ? auto_raw_capacity(cfg, s, cfg.echo_every) : 16),
        raw_rtt_net_(cfg.echo_every ? auto_raw_capacity(cfg, s, cfg.echo_every) : 16) {}
  virtual ~PubStreamBase() = default;

  [[nodiscard]] const StreamSpec& spec() const noexcept { return spec_; }

  // --- Hilo writer ---
  virtual void publish(int64_t planned_ns, int64_t woke_ns, bool warmup) noexcept = 0;
  void add_overruns(uint64_t n) noexcept {
    wacc_.record([n](PubWriteAcc& a) { a.overruns += n; });
    overruns_ += n;
  }
  // --- Hilo eco ---
  virtual size_t drain_echo() noexcept = 0;
  // --- Hilo principal ---
  virtual void attach_echo(typename B::WaitSet& ws) = 0;
  [[nodiscard]] virtual int32_t writer_matched() const = 0;
  [[nodiscard]] virtual int32_t echo_matched() const = 0;
  [[nodiscard]] virtual bool loans_active() const = 0;
  virtual void close_writer(int64_t ack_timeout_ns) = 0;

  // Fila de ventana (hilo de estadísticas)
  void collect_window(Record& r, double window_s, const std::string& phase, double cpu_pct) {
    PhaseAgg& ph = phases_.at(phase);
    ph.seconds += window_s;
    ++ph.windows;
    ph.cpu_pct_sum += cpu_pct;
    wacc_.collect([&](const PubWriteAcc& a) {
      ph.msgs += a.samples;
      ph.bytes += a.bytes;
      ph.errors += a.write_errors;
      r.add("msgs", a.samples);
      r.add("msg_per_s", rate(static_cast<double>(a.samples), window_s), 1);
      r.add("bytes", a.bytes);
      r.add("mb_per_s", rate(static_cast<double>(a.bytes), window_s) / 1e6, 4);
      r.add("write_errors", a.write_errors);
      r.add("overruns", a.overruns);
      r.add("loan_fallbacks", a.loan_fallbacks);
      r.add_latency_us("write_call", summarize(a.write_call), false);
      r.add_latency_us("wakeup", summarize(a.wakeup_latency), false);
      window_active_ = a.samples > 0;
    });
    eacc_.collect([&](const PubEchoAcc& a) {
      ph.lat_a.merge(a.rtt);
      ph.lat_b.merge(a.rtt_net);
      r.add("echoes", a.echoes);
      const LatencySummary rtt = summarize(a.rtt);
      const LatencySummary net = summarize(a.rtt_net);
      r.add_latency_us("rtt", rtt, false);
      r.add_latency_us("rtt_net", net, false);
      if (net.count) {
        r.add("one_way_est_p50_us", static_cast<double>(net.p50) / 2e3);
      } else {
        r.add_empty("one_way_est_p50_us");
      }
      if (a.offset_valid) {
        last_offset_ns_ = a.offset_ns;
        last_offset_delay_ns_ = a.offset_delay_ns;
      }
      r.add("offset_us", static_cast<double>(last_offset_ns_) / 1e3);
      r.add("offset_delay_us", static_cast<double>(last_offset_delay_ns_) / 1e3);
    });
    const uint64_t dl = wstatus_.offered_deadline_missed.load();
    const uint64_t ll = wstatus_.liveliness_lost.load();
    const uint64_t d_dl = dl - last_dl_;
    const uint64_t d_ll = ll - last_ll_;
    last_dl_ = dl;
    last_ll_ = ll;
    if (window_active_) {
      active_dl_ += d_dl;
      active_ll_ += d_ll;
      active_s_ += window_s;
      ph.deadline_missed += d_dl;
    }
    r.add("deadline_missed", d_dl);
    r.add("deadline_miss_rate", deadline_miss_rate(d_dl, window_s, spec_.instances, spec_.qos.deadline_ns.value_or(0)), 6);
    r.add("liveliness_lost", d_ll);
    r.add("incompatible_qos", wstatus_.offered_incompatible_qos.load());
    r.add("matched", static_cast<int64_t>(wstatus_.matched.load()));
  }

  // Resumen final (tras unir hilos)
  void summary(Record& r, double measured_s) {
    r.add("sent", sent_);
    r.add("sent_bytes", sent_bytes_);
    r.add("first_measured_seq", first_measured_seq_);
    r.add("last_seq", seq_ ? seq_ - 1 : 0);
    r.add("msg_per_s", rate(static_cast<double>(sent_), measured_s), 1);
    r.add("mb_per_s", rate(static_cast<double>(sent_bytes_), measured_s) / 1e6, 4);
    r.add("write_errors", write_errors_);
    r.add("overruns", overruns_);
    r.add("loan_fallbacks", loan_fallbacks_);
    r.add("loans_active", static_cast<int64_t>(loans_active() ? 1 : 0));
    r.add("echoes", echoes_);
    r.add("echo_ratio", sent_ ? static_cast<double>(echoes_) * std::max(1u, echo_every_) / static_cast<double>(sent_) : 0.0, 4);
    const LatencySummary rtt = raw_rtt_.complete() ? raw_rtt_.summarize_exact() : summarize(full_rtt_);
    const LatencySummary net = raw_rtt_net_.complete() ? raw_rtt_net_.summarize_exact() : summarize(full_rtt_net_);
    r.add_latency_us("rtt", rtt);
    r.add_latency_us("rtt_net", net);
    r.add("one_way_est_p50_us", net.count ? static_cast<double>(net.p50) / 2e3 : 0.0);
    r.add("one_way_est_p99_us", net.count ? static_cast<double>(net.p99) / 2e3 : 0.0);
    r.add("rtt_exact", static_cast<int64_t>(raw_rtt_.complete() ? 1 : 0));
    r.add("offset_us", static_cast<double>(offset_.offset_ns()) / 1e3);
    r.add("offset_delay_us", static_cast<double>(offset_.delay_ns()) / 1e3);
    r.add_latency_us("write_call", summarize(full_write_call_));
    r.add_latency_us("wakeup", summarize(full_wakeup_));
    r.add("deadline_missed", active_dl_);
    r.add("deadline_miss_rate",
          deadline_miss_rate(active_dl_, active_s_, spec_.instances, spec_.qos.deadline_ns.value_or(0)), 6);
    r.add("liveliness_lost", active_ll_);
    r.add("incompatible_qos", wstatus_.offered_incompatible_qos.load());
  }

  // Filas por fase (baseline / noise / recovery...)
  void phase_records(const BenchConfig& cfg, std::vector<Record>& out) const {
    for (const auto& [name, ph] : phases_.entries()) {
      Record r;
      add_identity(r, cfg, to_string(spec_.kind), name);
      r.add("windows", ph->windows);
      r.add("seconds", ph->seconds, 3);
      r.add("msgs", ph->msgs);
      r.add("msg_per_s", rate(static_cast<double>(ph->msgs), ph->seconds), 1);
      r.add("mb_per_s", rate(static_cast<double>(ph->bytes), ph->seconds) / 1e6, 4);
      r.add("lost", uint64_t{0});
      r.add("loss_pct", 0.0, 4);
      r.add("errors", ph->errors);
      r.add("deadline_missed", ph->deadline_missed);
      r.add("deadline_miss_rate",
            deadline_miss_rate(ph->deadline_missed, ph->seconds, spec_.instances, spec_.qos.deadline_ns.value_or(0)), 6);
      r.add("cpu_pct_avg", ph->windows ? ph->cpu_pct_sum / static_cast<double>(ph->windows) : 0.0, 2);
      r.add("latency_metric", "rtt", true);
      r.add_latency_us("lat", summarize(ph->lat_a));
      r.add_latency_us("lat2", summarize(ph->lat_b));
      out.push_back(std::move(r));
    }
  }

  [[nodiscard]] uint64_t sent() const noexcept { return sent_; }
  [[nodiscard]] const ClockOffsetEstimator& offset() const noexcept { return offset_; }
  [[nodiscard]] const LatencyHistogram& full_rtt() const noexcept { return full_rtt_; }

 protected:
  // Registro de una escritura (hilo writer).
  void account_write(bool warmup, bool ok, bool fallback, uint32_t bytes, int64_t call_ns, int64_t wake_ns,
                     uint64_t seq) noexcept {
    wacc_.record([&](PubWriteAcc& a) {
      if (!warmup) {
        ++a.samples;
        a.bytes += bytes;
        a.write_call.record(call_ns);
        a.wakeup_latency.record(wake_ns);
      }
      if (!ok) ++a.write_errors;
      if (fallback) ++a.loan_fallbacks;
    });
    if (!warmup) {
      if (sent_ == 0) first_measured_seq_ = seq;
      ++sent_;
      sent_bytes_ += bytes;
      full_write_call_.record(call_ns);
      full_wakeup_.record(wake_ns);
    }
    if (!ok) ++write_errors_;
    if (fallback) ++loan_fallbacks_;
  }

  // Procesado de un eco (hilo eco): T1..T4 -> RTT, RTT neto y offset.
  template <class Echo>
  void account_echo(const Echo& e) noexcept {
    const int64_t t4_mono = now_mono_ns();
    const int64_t t4_rt = now_rt_ns();
    const auto& h = e.hdr();
    if (h.stream_id() != spec_.stream_id) return;  // eco dirigido a otro publicador
    const int64_t rtt = t4_mono - h.t_pub_tx_mono_ns();
    const int64_t turnaround = e.t_sub_tx_rt_ns() - e.t_sub_rx_rt_ns();
    const int64_t rtt_net = rtt - turnaround;
    offset_.add(h.t_pub_tx_rt_ns(), e.t_sub_rx_rt_ns(), e.t_sub_tx_rt_ns(), t4_rt);
    const bool warm = (h.flags() & SensorsTelemetry::BENCH_FLAG_WARMUP) != 0;
    const int64_t off = offset_.offset_ns();
    const int64_t delay = offset_.delay_ns();
    eacc_.record([&](PubEchoAcc& a) {
      if (!warm) {
        ++a.echoes;
        a.rtt.record(rtt);
        a.rtt_net.record(rtt_net);
      }
      a.offset_ns = off;
      a.offset_delay_ns = delay;
      a.offset_valid = true;
    });
    if (!warm) {
      ++echoes_;
      full_rtt_.record(rtt);
      full_rtt_net_.record(rtt_net);
      raw_rtt_.push(rtt);
      raw_rtt_net_.push(rtt_net);
    }
  }

  StreamSpec spec_;
  uint32_t echo_every_ = 1;
  uint32_t node_id_ = 1;
  uint64_t seq_ = 0;
  WriterStatusCounters wstatus_;
  ReaderStatusCounters echo_status_;
  ClockOffsetEstimator offset_;

 private:
  DoubleBuffered<PubWriteAcc> wacc_;
  DoubleBuffered<PubEchoAcc> eacc_;
  // Acumulados de toda la ejecución (cada uno lo escribe un único hilo RT)
  LatencyHistogram full_write_call_, full_wakeup_, full_rtt_, full_rtt_net_;
  RawSeries raw_rtt_, raw_rtt_net_;
  uint64_t sent_ = 0, sent_bytes_ = 0, write_errors_ = 0, loan_fallbacks_ = 0, overruns_ = 0, echoes_ = 0;
  uint64_t first_measured_seq_ = 0;
  // Estado del hilo de estadísticas
  uint64_t last_dl_ = 0, last_ll_ = 0, active_dl_ = 0, active_ll_ = 0;
  double active_s_ = 0;
  bool window_active_ = false;
  int64_t last_offset_ns_ = 0, last_offset_delay_ns_ = 0;
  PhaseTable phases_;
};

// ---------------------------------------------------------------------------
template <class B, class T, class Gen>
class PubStream final : public PubStreamBase<B> {
  using Base = PubStreamBase<B>;
  using Echo = SensorsTelemetry::EchoReply;

 public:
  PubStream(B& backend, const StreamSpec& s, const BenchConfig& cfg, Gen gen)
      : Base(s, cfg), gen_(std::move(gen)) {
    this->echo_every_ = cfg.echo_every;
    this->node_id_ = cfg.node_id;
    if constexpr (requires(T& x) { x.samples().reserve(1); }) SonarGenerator::prepare(sample_);
    writer_ = backend.template create_writer<T>(s, s.topic, s.qos, s.qos_profile, &this->wstatus_);
    if (cfg.echo_every > 0) {
      echo_reader_ = backend.template create_reader<Echo>(s, s.echo_topic, make_echo_qos(s), "", &this->echo_status_);
    }
    use_loans_ = cfg.loans && writer_->loans_supported();
  }

  void publish(int64_t planned_ns, int64_t woke_ns, bool warmup) noexcept override {
    const uint32_t idx = next_instance_;
    next_instance_ = (next_instance_ + 1) % gen_.instances();
    T* s = &sample_;
    bool loaned = false, fallback = false;
    if (use_loans_) {
      if (T* l = writer_->loan()) {
        s = l;
        loaned = true;
      } else {
        fallback = true;
      }
    }
    const uint32_t bytes = gen_.fill(*s, idx, now_rt_ns());
    const uint64_t seq = this->seq_++;
    uint32_t flags = 0;
    if (warmup) flags |= SensorsTelemetry::BENCH_FLAG_WARMUP;
    if (this->echo_every_ != 0 && (seq % this->echo_every_) == 0) flags |= SensorsTelemetry::BENCH_FLAG_ECHO_REQUEST;
    int64_t offset = 0;
    if (this->offset_.valid()) {
      offset = this->offset_.offset_ns();
      flags |= SensorsTelemetry::BENCH_FLAG_OFFSET_VALID;
    }
    auto& h = s->header();
    h.seq(seq);
    h.stream_id(this->spec_.stream_id);
    h.payload_bytes(bytes);
    h.flags(flags);
    h.writer_node_id(this->node_id_);
    h.clock_offset_est_ns(offset);
    // T1 lo más cerca posible de write()
    const int64_t t1_mono = now_mono_ns();
    h.t_pub_tx_mono_ns(t1_mono);
    h.t_pub_tx_rt_ns(now_rt_ns());
    const bool ok = loaned ? writer_->write_loaned(s) : writer_->write(*s);
    const int64_t t_after = now_mono_ns();
    this->account_write(warmup, ok, fallback, bytes, t_after - t1_mono, woke_ns - planned_ns, seq);
  }

  size_t drain_echo() noexcept override {
    if (!echo_reader_) return 0;
    return echo_reader_->take([this](const Echo& e) { this->account_echo(e); }, 64);
  }

  void attach_echo(typename B::WaitSet& ws) override {
    if (echo_reader_) ws.attach(*echo_reader_);
  }
  [[nodiscard]] int32_t writer_matched() const override { return writer_ ? writer_->matched() : 0; }
  [[nodiscard]] int32_t echo_matched() const override { return echo_reader_ ? echo_reader_->matched() : 0; }
  [[nodiscard]] bool loans_active() const override { return use_loans_; }
  void close_writer(int64_t ack_timeout_ns) override {
    if (writer_) {
      writer_->flush(ack_timeout_ns);
      writer_.reset();
    }
  }
  // El lector de eco debe destruirse antes que el WaitSet que lo referencia.
  void close_echo() { echo_reader_.reset(); }

 private:
  Gen gen_;
  T sample_{};
  uint32_t next_instance_ = 0;
  bool use_loans_ = false;
  std::unique_ptr<typename B::template Writer<T>> writer_;
  std::unique_ptr<typename B::template Reader<Echo>> echo_reader_;
};

template <class B>
std::unique_ptr<PubStreamBase<B>> make_pub_stream(B& backend, const StreamSpec& s, const BenchConfig& cfg) {
  const uint64_t seed = cfg.seed * 1000003ULL + s.stream_id;
  switch (s.kind) {
    case SensorKind::Radar:
      return std::make_unique<PubStream<B, SensorsTelemetry::RadarTrack, RadarGenerator>>(
          backend, s, cfg, RadarGenerator(s.instances, cfg.node_id, seed));
    case SensorKind::Sonar:
      return std::make_unique<PubStream<B, SensorsTelemetry::SonarPing, SonarGenerator>>(
          backend, s, cfg, SonarGenerator(s.instances, s.sonar_min_bytes, s.sonar_max_bytes, seed));
    case SensorKind::Weather:
      return std::make_unique<PubStream<B, SensorsTelemetry::WeatherStation, WeatherGenerator>>(
          backend, s, cfg, WeatherGenerator(s.instances, seed));
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
template <class B>
class PublisherApp {
 public:
  explicit PublisherApp(BenchConfig cfg) : cfg_(std::move(cfg)) {}

  int run() {
    install_signal_handlers();
    ctx_ = prepare_process(cfg_);
    log_info(cfg_, cfg_.describe());

    try {
      backend_ = std::make_unique<B>(cfg_);
      for (const auto& s : cfg_.streams) streams_.push_back(make_pub_stream<B>(*backend_, s, cfg_));
      echo_ws_ = backend_->create_waitset();
      for (auto& s : streams_) s->attach_echo(*echo_ws_);
    } catch (const std::exception& e) {
      log_warn(std::string("error creando entidades DDS: ") + e.what());
      return 2;
    }
    log_info(cfg_, "middleware: " + backend_->version());
    for (auto& s : streams_) {
      if (cfg_.loans && !s->loans_active()) {
        log_warn(std::string("loans no disponibles para ") + to_string(s->spec().kind) + " (tipo no plain o transporte sin SHM)");
      }
    }
    enter_housekeeping(cfg_);

    if (!wait_for_matching()) {
      log_warn("timeout esperando suscriptores (--match-timeout)");
      if (stop_requested()) return 130;
    }
    apply_dscp(true);

    if (cfg_.prom_port > 0) {
      std::string err;
      if (!prom_.start(cfg_.prom_port, &err)) log_warn("exportador Prometheus: " + err);
    }
    timeseries_.open(ctx_.output_prefix + "_timeseries.csv");

    // Hilos RT
    const int64_t t0 = now_mono_ns() + 50 * kNsPerMs;
    measure_start_ns_ = t0 + static_cast<int64_t>(cfg_.warmup_s * 1e9);
    end_ns_ = measure_start_ns_ + static_cast<int64_t>(cfg_.duration_s * 1e9);
    std::thread echo_thread([this] { echo_loop(); });
    std::thread writer_thread([this, t0] { writer_loop(t0); });

    // Bucle de estadísticas (hilo principal, no-RT)
    HostSampler host(cfg_.iface);
    ProcSample proc_start{};
    int64_t next_window = t0 + cfg_.window_ns;
    uint64_t window_idx = 0;
    bool measuring = false;
    bool threads_reported = false;
    while (!writer_done_.load()) {
      sleep_until_mono_ns(std::min(next_window, now_mono_ns() + 100 * kNsPerMs));
      if (now_mono_ns() >= next_window) {
        if (!measuring && next_window > measure_start_ns_) {
          measuring = true;
          proc_start = read_proc_sample();
        }
        emit_window(host, window_idx++, next_window, t0);
        next_window += cfg_.window_ns;
        // Inventario de hilos con los hilos RT aún vivos (última ventana de medida)
        if (!threads_reported && next_window + cfg_.window_ns >= end_ns_) {
          write_thread_report(ctx_.output_prefix + "_threads.csv", cfg_);
          threads_reported = true;
        }
      }
    }
    writer_thread.join();
    if (!threads_reported) write_thread_report(ctx_.output_prefix + "_threads.csv", cfg_);
    const ProcSample proc_end = read_proc_sample();

    // Cierre ordenado: esperar ACKs y eliminar writers (evita falsos deadline
    // misses en el lector), dejar drenar ecos y parar el hilo de eco.
    for (auto& s : streams_) s->close_writer(1 * kNsPerSec);
    const int64_t drain_end = now_mono_ns() + static_cast<int64_t>(cfg_.drain_s * 1e9);
    while (now_mono_ns() < drain_end && !stop_requested()) sleep_until_mono_ns(now_mono_ns() + 50 * kNsPerMs);
    echo_stop_.store(true);
    echo_thread.join();
    emit_window(host, window_idx++, now_mono_ns(), t0);

    write_summary(measuring ? proc_start : proc_end, proc_end);
    write_meta_json(ctx_.output_prefix + "_meta.json", cfg_, ctx_, backend_->version());

    // Destrucción en orden: WaitSet -> lectores de eco -> backend
    echo_ws_.reset();
    streams_.clear();
    backend_.reset();
    prom_.stop();
    return 0;
  }

 private:
  bool wait_for_matching() {
    const int64_t deadline = now_mono_ns() + static_cast<int64_t>(cfg_.match_timeout_s * 1e9);
    int64_t next_log = 0;
    while (!stop_requested()) {
      bool all = true;
      std::string status;
      for (auto& s : streams_) {
        const int32_t w = s->writer_matched();
        const int32_t e = s->echo_matched();
        all = all && w >= cfg_.expected_matches && (cfg_.echo_every == 0 || e >= cfg_.expected_matches);
        status += std::string(" ") + to_string(s->spec().kind) + "[w=" + std::to_string(w) + " echo=" + std::to_string(e) + "]";
      }
      if (all) {
        log_info(cfg_, "emparejamiento completo:" + status);
        return true;
      }
      const int64_t now = now_mono_ns();
      if (now >= deadline) return false;
      if (now >= next_log) {
        log_info(cfg_, "esperando suscriptores..." + status);
        next_log = now + kNsPerSec;
      }
      sleep_until_mono_ns(now + 20 * kNsPerMs);
    }
    return false;
  }

  void writer_loop(int64_t t0) {
    std::string err;
    if (!rt::configure_current_thread("bench.writer", cfg_.rt.app_cpus, cfg_.rt.enable_fifo ? cfg_.rt.app_priority : 0,
                                      cfg_.rt.prefault_stack_kb, &err)) {
      log_warn("hilo writer: " + err);
    }
    const size_t n = streams_.size();
    std::vector<int64_t> next(n), period(n);
    for (size_t i = 0; i < n; ++i) {
      period[i] = streams_[i]->spec().period_ns();
      next[i] = t0 + static_cast<int64_t>(i) * period[i] / static_cast<int64_t>(n);  // desfase entre flujos
    }
    while (!stop_requested()) {
      size_t k = 0;
      for (size_t i = 1; i < n; ++i) {
        if (next[i] < next[k]) k = i;
      }
      const int64_t due = next[k];
      if (due >= end_ns_) break;
      sleep_until_mono_ns(due);
      const int64_t woke = now_mono_ns();
      streams_[k]->publish(due, woke, due < measure_start_ns_);
      next[k] += period[k];
      const int64_t now = now_mono_ns();
      if (now > next[k] + period[k]) {  // retraso > 1 periodo: se saltan ranuras (sin ráfagas)
        const int64_t skipped = (now - next[k]) / period[k];
        next[k] += skipped * period[k];
        streams_[k]->add_overruns(static_cast<uint64_t>(skipped));
      }
    }
    writer_done_.store(true);
  }

  void echo_loop() {
    std::string err;
    if (!rt::configure_current_thread("bench.echo", cfg_.rt.app_cpus, cfg_.rt.enable_fifo ? cfg_.rt.echo_priority : 0,
                                      cfg_.rt.prefault_stack_kb, &err)) {
      log_warn("hilo eco: " + err);
    }
    if (cfg_.echo_every == 0) {
      while (!echo_stop_.load()) sleep_until_mono_ns(now_mono_ns() + 100 * kNsPerMs);
      return;
    }
    while (!echo_stop_.load()) {
      echo_ws_->wait(50 * kNsPerMs);
      for (auto& s : streams_) s->drain_echo();
    }
  }

  void apply_dscp(bool log) {
    if (cfg_.dscp == -2) return;
    int32_t max_tp = 0;
    for (const auto& s : cfg_.streams) max_tp = std::max(max_tp, s.qos.transport_priority.value_or(0));
    const DscpMapping m = map_transport_priority(max_tp);
    const int dscp = cfg_.dscp >= 0 ? cfg_.dscp : m.dscp;
    const int prio = cfg_.so_priority >= 0 ? cfg_.so_priority : m.so_priority;
    if (dscp == 0 && prio == 0) return;
    const TagResult r = tag_process_udp_sockets(dscp, prio);
    if (log) {
      log_info(cfg_, "DSCP " + std::to_string(dscp) + " (TRANSPORT_PRIORITY max=" + std::to_string(max_tp) + " -> " +
                         m.phb + ", SO_PRIORITY=" + std::to_string(prio) + ") aplicado a " +
                         std::to_string(r.sockets_tagged) + " sockets UDP" +
                         (r.sockets_failed ? " (" + std::to_string(r.sockets_failed) + " fallos: " + r.last_error + ")" : ""));
    }
  }

  void emit_window(HostSampler& host, uint64_t idx, int64_t window_end_mono, int64_t t0) {
    host.sample();
    const double window_s = host.window_s();
    const bool warm = window_end_mono <= measure_start_ns_;
    const std::string phase = warm ? std::string("warmup") : read_phase(cfg_.phase_file, "run");
    std::vector<Record> records;
    for (auto& s : streams_) {
      Record r;
      add_identity(r, cfg_, to_string(s->spec().kind), phase);
      r.add("window_idx", idx);
      r.add("t_wall_s", static_cast<double>(now_rt_ns()) / 1e9, 3);
      r.add("t_rel_s", static_cast<double>(window_end_mono - measure_start_ns_) / 1e9, 3);
      r.add("window_s", window_s, 3);
      r.add("warmup", static_cast<int64_t>(warm ? 1 : 0));
      s->collect_window(r, window_s, phase, host.proc().cpu_pct);
      host.append(r);
      timeseries_.write(r);
      records.push_back(std::move(r));
    }
    publish_prometheus(records);
    apply_dscp(false);  // cubre sockets creados tras el descubrimiento
    if (!cfg_.quiet && !warm) print_window(records, idx);
    (void)t0;
  }

  void print_window(const std::vector<Record>& records, uint64_t idx) {
    for (const auto& r : records) {
      std::string line = "w" + std::to_string(idx);
      for (const auto& f : r.fields()) {
        if (f.key == "stream" || f.key == "msg_per_s" || f.key == "rtt_p50_us" || f.key == "rtt_p99_us" ||
            f.key == "offset_us" || f.key == "deadline_missed" || f.key == "write_errors" || f.key == "cpu_pct") {
          line += " " + f.key + "=" + f.value;
        }
      }
      log_info(cfg_, line);
    }
  }

  void publish_prometheus(const std::vector<Record>& records) {
    if (cfg_.prom_port <= 0 && cfg_.prom_textfile.empty()) return;
    const std::string text = to_prometheus(records);
    if (cfg_.prom_port > 0) prom_.set_text(text);
    if (!cfg_.prom_textfile.empty()) write_file_atomic(cfg_.prom_textfile, text);
  }

  void write_summary(const ProcSample& p0, const ProcSample& p1) {
    CsvSink csv;
    csv.open(ctx_.output_prefix + "_summary.csv");
    const ProcDelta pd = diff(p0, p1);
    const double measured_s = cfg_.duration_s;
    std::vector<Record> records;
    for (auto& s : streams_) {
      Record r;
      add_identity(r, cfg_, to_string(s->spec().kind), "summary");
      r.add("middleware_version", backend_->version(), true);
      r.add("reliability", s->spec().qos.reliability ? to_string(*s->spec().qos.reliability) : "profile", true);
      r.add("history", s->spec().qos.history ? to_string(*s->spec().qos.history) : "profile", true);
      r.add("depth", static_cast<int64_t>(s->spec().qos.depth.value_or(-1)));
      r.add("deadline_ms", static_cast<double>(s->spec().qos.deadline_ns.value_or(0)) / 1e6);
      r.add("transport_priority", static_cast<int64_t>(s->spec().qos.transport_priority.value_or(0)));
      r.add("rate_hz", s->spec().aggregate_rate_hz(), 1);
      r.add("instances", s->spec().instances);
      r.add("duration_s", measured_s, 1);
      s->summary(r, measured_s);
      r.add("cpu_pct_avg", pd.cpu_pct, 2);
      r.add("rss_kb", p1.rss_kb);
      r.add("hwm_kb", p1.hwm_kb);
      r.add("invol_ctxsw", pd.invol_ctxsw);
      r.add("majflt", pd.majflt);
      csv.write(r);
      records.push_back(r);
      if (!cfg_.quiet) print_summary(r);
    }
    write_file_atomic(ctx_.output_prefix + "_summary.prom", to_prometheus(records, "dds_bench_summary_"));
    write_phases();
    log_info(cfg_, "resultados en " + ctx_.output_prefix + "_*.csv");
  }

  void write_phases() {
    std::vector<Record> rows;
    for (auto& s : streams_) s->phase_records(cfg_, rows);
    CsvSink csv;
    csv.open(ctx_.output_prefix + "_phases.csv");
    for (const auto& r : rows) csv.write(r);
  }

  void print_summary(const Record& r) {
    std::string line = "RESUMEN";
    for (const auto& f : r.fields()) {
      static const char* keys[] = {"stream", "sent", "msg_per_s", "mb_per_s", "rtt_mean_us", "rtt_std_us", "rtt_p50_us",
                                   "rtt_p95_us", "rtt_p99_us", "rtt_p999_us", "rtt_max_us", "one_way_est_p50_us",
                                   "offset_us", "write_errors", "overruns", "deadline_missed", "cpu_pct_avg", "rss_kb"};
      for (const char* k : keys) {
        if (f.key == k) line += " " + f.key + "=" + f.value;
      }
    }
    log_info(cfg_, line);
  }

  BenchConfig cfg_;
  ProcessContext ctx_;
  std::unique_ptr<B> backend_;
  std::vector<std::unique_ptr<PubStreamBase<B>>> streams_;
  std::unique_ptr<typename B::WaitSet> echo_ws_;
  PromHttpExporter prom_;
  CsvSink timeseries_;
  int64_t measure_start_ns_ = 0;
  int64_t end_ns_ = 0;
  std::atomic<bool> writer_done_{false};
  std::atomic<bool> echo_stop_{false};
};

}  // namespace bench
