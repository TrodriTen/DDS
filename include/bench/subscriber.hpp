// SPDX-License-Identifier: Apache-2.0
// Motor genérico del SUSCRIPTOR (Data Reader), plantilla sobre el backend DDS.
//
// Hilos:
//   bench.reader (SCHED_FIFO app_priority): WaitSet sobre todos los lectores;
//       por muestra: T2, eco inmediato (T3) si se solicita, E2E bruta y corregida,
//       seguimiento de secuencia (pérdidas/duplicados/desorden).
//   main/bench.stats (SCHED_OTHER): ventanas, estados DDS (deadline, liveliness,
//       sample lost/rejected), CPU/RSS, CSV y Prometheus.
#pragma once

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "bench/clock.hpp"
#include "bench/config.hpp"
#include "bench/harness_common.hpp"
#include "bench/histogram.hpp"
#include "bench/metrics.hpp"
#include "bench/publisher.hpp"  // make_echo_qos
#include "bench/raw_series.hpp"
#include "bench/seq_tracker.hpp"
#include "bench/synthetic.hpp"
#include "bench/window.hpp"

namespace bench {

template <class B>
class SubStreamBase {
 public:
  SubStreamBase(const StreamSpec& s, const BenchConfig& cfg)
      : spec_(s), raw_e2e_(auto_raw_capacity(cfg, s)), raw_e2e_corr_(auto_raw_capacity(cfg, s)) {}
  virtual ~SubStreamBase() = default;

  [[nodiscard]] const StreamSpec& spec() const noexcept { return spec_; }
  virtual size_t drain() noexcept = 0;
  virtual void attach(typename B::WaitSet& ws) = 0;
  [[nodiscard]] int32_t matched() const noexcept { return status_.matched.load(); }
  [[nodiscard]] int32_t matched_peak() const noexcept { return status_.matched_peak.load(); }
  [[nodiscard]] bool has_data() const noexcept { return any_data_.load(std::memory_order_relaxed); }

  void collect_window(Record& r, double window_s) {
    uint64_t samples = 0;
    acc_.collect([&](const SubWindowAcc& a) {
      samples = a.samples;
      r.add("msgs", a.samples);
      r.add("warmup_msgs", a.warmup_samples);
      r.add("msg_per_s", rate(static_cast<double>(a.samples), window_s), 1);
      r.add("bytes", a.bytes);
      r.add("mb_per_s", rate(static_cast<double>(a.bytes), window_s) / 1e6, 4);
      uint64_t d_lost = 0, d_dup = 0, d_ooo = 0;
      if (a.seq_valid) {
        d_lost = a.seq_lost >= last_lost_ ? a.seq_lost - last_lost_ : 0;
        d_dup = a.seq_dup - last_dup_;
        d_ooo = a.seq_ooo - last_ooo_;
        last_lost_ = a.seq_lost;
        last_dup_ = a.seq_dup;
        last_ooo_ = a.seq_ooo;
      }
      r.add("lost", d_lost);
      r.add("loss_pct", a.samples + d_lost ? 100.0 * static_cast<double>(d_lost) / static_cast<double>(a.samples + d_lost) : 0.0, 4);
      r.add("dup", d_dup);
      r.add("ooo", d_ooo);
      const LatencySummary e2e = summarize(a.e2e);
      r.add_latency_us("e2e", e2e, false);
      r.add_latency_us("e2e_corr", summarize(a.e2e_corr), false);
      r.add("e2e_negative", e2e.negative);
      r.add("echo_sent", a.echo_sent);
      r.add("echo_failed", a.echo_failed);
    });
    const uint64_t dl = status_.requested_deadline_missed.load();
    const uint64_t sl = status_.sample_lost.load();
    const uint64_t sr = status_.sample_rejected.load();
    const uint64_t lv = status_.liveliness_not_alive_changes.load();
    const uint64_t d_dl = dl - last_dl_, d_sl = sl - last_sl_, d_sr = sr - last_sr_, d_lv = lv - last_lv_;
    last_dl_ = dl;
    last_sl_ = sl;
    last_sr_ = sr;
    last_lv_ = lv;
    if (samples > 0) {  // sólo ventanas con flujo activo cuentan para el resumen
      active_dl_ += d_dl;
      active_sl_ += d_sl;
      active_sr_ += d_sr;
      active_lv_ += d_lv;
      active_s_ += window_s;
    }
    r.add("deadline_missed", d_dl);
    r.add("deadline_miss_rate", deadline_miss_rate(d_dl, window_s, spec_.instances, spec_.qos.deadline_ns.value_or(0)), 6);
    r.add("sample_lost", d_sl);
    r.add("sample_rejected", d_sr);
    r.add("liveliness_not_alive", d_lv);
    r.add("incompatible_qos", status_.requested_incompatible_qos.load());
    r.add("matched", static_cast<int64_t>(status_.matched.load()));
  }

  void summary(Record& r) {
    const double span_s = last_rx_mono_ > first_rx_mono_ ? static_cast<double>(last_rx_mono_ - first_rx_mono_) / 1e9 : 0.0;
    uint64_t expected = 0, lost = 0, dup = 0, ooo = 0;
    for (size_t i = 0; i < n_trackers_; ++i) {
      expected += trackers_[i].tracker.expected();
      lost += trackers_[i].tracker.lost();
      dup += trackers_[i].tracker.duplicates();
      ooo += trackers_[i].tracker.out_of_order();
    }
    r.add("publishers", static_cast<uint64_t>(n_trackers_));
    r.add("received", received_);
    r.add("received_bytes", received_bytes_);
    r.add("expected", expected);
    r.add("lost", lost);
    r.add("loss_pct", expected ? 100.0 * static_cast<double>(lost) / static_cast<double>(expected) : 0.0, 4);
    r.add("dup", dup);
    r.add("ooo", ooo);
    r.add("span_s", span_s, 3);
    r.add("msg_per_s", rate(static_cast<double>(received_), span_s), 1);
    r.add("mb_per_s", rate(static_cast<double>(received_bytes_), span_s) / 1e6, 4);
    const LatencySummary e2e = raw_e2e_.complete() ? raw_e2e_.summarize_exact() : summarize(full_e2e_);
    const LatencySummary corr = raw_e2e_corr_.complete() ? raw_e2e_corr_.summarize_exact() : summarize(full_e2e_corr_);
    r.add_latency_us("e2e", e2e);
    r.add_latency_us("e2e_corr", corr);
    r.add("e2e_negative", e2e.negative);
    r.add("e2e_exact", static_cast<int64_t>(raw_e2e_.complete() ? 1 : 0));
    r.add("echo_sent", echo_sent_);
    r.add("echo_failed", echo_failed_);
    r.add("deadline_missed", active_dl_);
    r.add("deadline_miss_rate",
          deadline_miss_rate(active_dl_, active_s_, spec_.instances, spec_.qos.deadline_ns.value_or(0)), 6);
    r.add("sample_lost", active_sl_);
    r.add("sample_rejected", active_sr_);
    r.add("liveliness_not_alive", active_lv_);
    r.add("incompatible_qos", status_.requested_incompatible_qos.load());
  }

 protected:
  template <class T, class EchoW, class EchoS>
  void on_sample(const T& s, EchoW* echo_writer, EchoS& echo, bool echo_enabled, uint32_t node) noexcept {
    const int64_t t2 = now_rt_ns();
    const auto& h = s.header();
    const uint32_t flags = h.flags();
    bool echoed = false, echo_ok = true;
    if (echo_enabled && echo_writer != nullptr && (flags & SensorsTelemetry::BENCH_FLAG_ECHO_REQUEST)) {
      echo.hdr(h);
      echo.t_sub_rx_rt_ns(t2);
      echo.responder_node_id(node);
      echo.t_sub_tx_rt_ns(now_rt_ns());  // T3 justo antes del write()
      echo_ok = echo_writer->write(echo);
      echoed = true;
    }
    any_data_.store(true, std::memory_order_relaxed);
    if (flags & SensorsTelemetry::BENCH_FLAG_WARMUP) {
      acc_.record([&](SubWindowAcc& a) {
        ++a.warmup_samples;
        if (echoed) echo_ok ? ++a.echo_sent : ++a.echo_failed;
      });
      return;
    }
    const int64_t t2_mono = now_mono_ns();
    const int64_t e2e = t2 - h.t_pub_tx_rt_ns();
    const bool offset_valid = (flags & SensorsTelemetry::BENCH_FLAG_OFFSET_VALID) != 0;
    const int64_t e2e_corr = e2e - h.clock_offset_est_ns();
    const uint32_t bytes = h.payload_bytes();
    SequenceTracker& tr = tracker_for(h.stream_id());
    tr.on_sample(h.seq());
    acc_.record([&](SubWindowAcc& a) {
      ++a.samples;
      a.bytes += bytes;
      a.e2e.record(e2e);
      if (offset_valid) a.e2e_corr.record(e2e_corr);
      if (echoed) echo_ok ? ++a.echo_sent : ++a.echo_failed;
      uint64_t lost = 0, dup = 0, ooo = 0;
      for (size_t i = 0; i < n_trackers_; ++i) {
        lost += trackers_[i].tracker.lost();
        dup += trackers_[i].tracker.duplicates();
        ooo += trackers_[i].tracker.out_of_order();
      }
      a.seq_valid = true;
      a.seq_lost = lost;
      a.seq_dup = dup;
      a.seq_ooo = ooo;
    });
    full_e2e_.record(e2e);
    raw_e2e_.push(e2e);
    if (offset_valid) {
      full_e2e_corr_.record(e2e_corr);
      raw_e2e_corr_.push(e2e_corr);
    }
    if (received_ == 0) first_rx_mono_ = t2_mono;
    last_rx_mono_ = t2_mono;
    ++received_;
    received_bytes_ += bytes;
    if (echoed) echo_ok ? ++echo_sent_ : ++echo_failed_;
  }

  StreamSpec spec_;
  ReaderStatusCounters status_;

 private:
  // Un SequenceTracker por publicador (stream_id), sin asignaciones dinámicas.
  static constexpr size_t kMaxPublishers = 8;
  struct Slot {
    uint32_t stream_id = 0;
    SequenceTracker tracker;
  };
  SequenceTracker& tracker_for(uint32_t stream_id) noexcept {
    for (size_t i = 0; i < n_trackers_; ++i) {
      if (trackers_[i].stream_id == stream_id) return trackers_[i].tracker;
    }
    if (n_trackers_ < kMaxPublishers) {
      trackers_[n_trackers_].stream_id = stream_id;
      return trackers_[n_trackers_++].tracker;
    }
    return overflow_;
  }

  DoubleBuffered<SubWindowAcc> acc_;
  std::array<Slot, kMaxPublishers> trackers_{};
  size_t n_trackers_ = 0;
  SequenceTracker overflow_;
  LatencyHistogram full_e2e_, full_e2e_corr_;
  RawSeries raw_e2e_, raw_e2e_corr_;
  uint64_t received_ = 0, received_bytes_ = 0, echo_sent_ = 0, echo_failed_ = 0;
  int64_t first_rx_mono_ = 0, last_rx_mono_ = 0;
  std::atomic<bool> any_data_{false};
  // Estado del hilo de estadísticas
  uint64_t last_lost_ = 0, last_dup_ = 0, last_ooo_ = 0;
  uint64_t last_dl_ = 0, last_sl_ = 0, last_sr_ = 0, last_lv_ = 0;
  uint64_t active_dl_ = 0, active_sl_ = 0, active_sr_ = 0, active_lv_ = 0;
  double active_s_ = 0;
};

template <class B, class T>
class SubStream final : public SubStreamBase<B> {
  using Base = SubStreamBase<B>;
  using Echo = SensorsTelemetry::EchoReply;

 public:
  SubStream(B& backend, const StreamSpec& s, const BenchConfig& cfg) : Base(s, cfg) {
    echo_enabled_ = cfg.echo_enabled;
    node_id_ = cfg.node_id;
    reader_ = backend.template create_reader<T>(s, s.topic, s.qos, s.qos_profile, &this->status_);
    if (echo_enabled_) {
      echo_writer_ = backend.template create_writer<Echo>(s, s.echo_topic, make_echo_qos(s), "", &echo_wstatus_);
    }
  }

  size_t drain() noexcept override {
    auto* ew = echo_writer_.get();
    return reader_->take([this, ew](const T& s) { this->on_sample(s, ew, echo_, echo_enabled_, node_id_); }, 64);
  }

  void attach(typename B::WaitSet& ws) override { ws.attach(*reader_); }

 private:
  bool echo_enabled_ = true;
  uint32_t node_id_ = 1;
  Echo echo_{};
  WriterStatusCounters echo_wstatus_;
  std::unique_ptr<typename B::template Reader<T>> reader_;
  std::unique_ptr<typename B::template Writer<Echo>> echo_writer_;
};

template <class B>
std::unique_ptr<SubStreamBase<B>> make_sub_stream(B& backend, const StreamSpec& s, const BenchConfig& cfg) {
  switch (s.kind) {
    case SensorKind::Radar: return std::make_unique<SubStream<B, SensorsTelemetry::RadarTrack>>(backend, s, cfg);
    case SensorKind::Sonar: return std::make_unique<SubStream<B, SensorsTelemetry::SonarPing>>(backend, s, cfg);
    case SensorKind::Weather: return std::make_unique<SubStream<B, SensorsTelemetry::WeatherStation>>(backend, s, cfg);
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
template <class B>
class SubscriberApp {
 public:
  explicit SubscriberApp(BenchConfig cfg) : cfg_(std::move(cfg)) {}

  int run() {
    install_signal_handlers();
    ctx_ = prepare_process(cfg_);
    log_info(cfg_, cfg_.describe());
    try {
      backend_ = std::make_unique<B>(cfg_);
      for (const auto& s : cfg_.streams) streams_.push_back(make_sub_stream<B>(*backend_, s, cfg_));
      ws_ = backend_->create_waitset();
      for (auto& s : streams_) s->attach(*ws_);
    } catch (const std::exception& e) {
      log_warn(std::string("error creando entidades DDS: ") + e.what());
      return 2;
    }
    log_info(cfg_, "middleware: " + backend_->version());
    enter_housekeeping(cfg_);
    apply_dscp();

    if (cfg_.prom_port > 0) {
      std::string err;
      if (!prom_.start(cfg_.prom_port, &err)) log_warn("exportador Prometheus: " + err);
    }
    timeseries_.open(ctx_.output_prefix + "_timeseries.csv");

    std::thread reader([this] { reader_loop(); });

    const int64_t t_start = now_mono_ns();
    const int64_t hard_limit = t_start + static_cast<int64_t>((cfg_.match_timeout_s + cfg_.warmup_s + cfg_.duration_s + 60.0) * 1e9);
    HostSampler host(cfg_.iface);
    ProcSample proc_start = read_proc_sample();
    bool started = false;
    int64_t next_window = 0, writers_gone_at = 0, next_log = 0;
    bool threads_reported = false;
    uint64_t window_idx = 0;
    while (!stop_requested()) {
      const int64_t now = now_mono_ns();
      if (now > hard_limit) {
        log_warn("límite de tiempo alcanzado");
        break;
      }
      if (!started) {
        bool any = false;
        for (auto& s : streams_) any = any || s->has_data();
        if (any) {
          started = true;
          next_window = now + cfg_.window_ns;
          host.sample();
          proc_start = read_proc_sample();
          log_info(cfg_, "recibiendo datos");
        } else {
          if (now >= next_log) {
            std::string st;
            for (auto& s : streams_) st += std::string(" ") + to_string(s->spec().kind) + "[matched=" + std::to_string(s->matched()) + "]";
            log_info(cfg_, "esperando publicadores..." + st);
            next_log = now + 2 * kNsPerSec;
          }
          if (now > t_start + static_cast<int64_t>(cfg_.match_timeout_s * 1e9) && cfg_.match_timeout_s > 0) {
            bool matched_any = false;
            for (auto& s : streams_) matched_any = matched_any || s->matched() > 0;
            if (!matched_any) {
              log_warn("timeout sin publicadores");
              break;
            }
          }
          sleep_until_mono_ns(now + 50 * kNsPerMs);
          continue;
        }
      }
      sleep_until_mono_ns(std::min(next_window, now_mono_ns() + 100 * kNsPerMs));
      if (now_mono_ns() >= next_window) {
        emit_window(host, window_idx++);
        next_window += cfg_.window_ns;
      }
      // Fin: todos los flujos que llegaron a emparejar se han quedado sin writers
      bool any_matched = false, all_gone = true;
      for (auto& s : streams_) {
        if (s->matched_peak() > 0) {
          any_matched = true;
          all_gone = all_gone && s->matched() == 0;
        }
      }
      if (any_matched && all_gone) {
        if (writers_gone_at == 0) {
          writers_gone_at = now_mono_ns();
          write_thread_report(ctx_.output_prefix + "_threads.csv", cfg_);  // hilo lector aún vivo
          threads_reported = true;
        }
        if (now_mono_ns() - writers_gone_at > static_cast<int64_t>(cfg_.drain_s * 1e9)) break;
      }
    }
    reader_stop_.store(true);
    reader.join();
    const ProcSample proc_end = read_proc_sample();
    if (started) emit_window(host, window_idx++);

    write_summary(proc_start, proc_end);
    if (!threads_reported) write_thread_report(ctx_.output_prefix + "_threads.csv", cfg_);
    write_meta_json(ctx_.output_prefix + "_meta.json", cfg_, ctx_, backend_->version());
    ws_.reset();
    streams_.clear();
    backend_.reset();
    prom_.stop();
    return 0;
  }

 private:
  void reader_loop() {
    std::string err;
    if (!rt::configure_current_thread("bench.reader", cfg_.rt.app_cpus, cfg_.rt.enable_fifo ? cfg_.rt.app_priority : 0,
                                      cfg_.rt.prefault_stack_kb, &err)) {
      log_warn("hilo lector: " + err);
    }
    while (!reader_stop_.load(std::memory_order_relaxed)) {
      ws_->wait(50 * kNsPerMs);
      for (auto& s : streams_) s->drain();
    }
  }

  void apply_dscp() {
    if (cfg_.dscp == -2) return;
    int32_t max_tp = 0;
    for (const auto& s : cfg_.streams) max_tp = std::max(max_tp, s.qos.transport_priority.value_or(0));
    const DscpMapping m = map_transport_priority(max_tp);
    const int dscp = cfg_.dscp >= 0 ? cfg_.dscp : m.dscp;
    const int prio = cfg_.so_priority >= 0 ? cfg_.so_priority : m.so_priority;
    if (dscp == 0 && prio == 0) return;
    tag_process_udp_sockets(dscp, prio);  // tráfico de eco y ACKNACK del lector
  }

  void emit_window(HostSampler& host, uint64_t idx) {
    host.sample();
    const double window_s = host.window_s();
    const std::string phase = read_phase(cfg_.phase_file, "run");
    std::vector<Record> records;
    for (auto& s : streams_) {
      Record r;
      add_identity(r, cfg_, to_string(s->spec().kind), phase);
      r.add("window_idx", idx);
      r.add("t_wall_s", static_cast<double>(now_rt_ns()) / 1e9, 3);
      r.add("window_s", window_s, 3);
      s->collect_window(r, window_s);
      host.append(r);
      timeseries_.write(r);
      records.push_back(std::move(r));
    }
    if (cfg_.prom_port > 0 || !cfg_.prom_textfile.empty()) {
      const std::string text = to_prometheus(records);
      if (cfg_.prom_port > 0) prom_.set_text(text);
      if (!cfg_.prom_textfile.empty()) write_file_atomic(cfg_.prom_textfile, text);
    }
    apply_dscp();
    if (!cfg_.quiet) {
      for (const auto& r : records) {
        std::string line = "w" + std::to_string(idx);
        for (const auto& f : r.fields()) {
          if (f.key == "stream" || f.key == "msg_per_s" || f.key == "lost" || f.key == "e2e_p50_us" ||
              f.key == "e2e_p99_us" || f.key == "e2e_corr_p50_us" || f.key == "deadline_missed" || f.key == "cpu_pct") {
            line += " " + f.key + "=" + f.value;
          }
        }
        log_info(cfg_, line);
      }
    }
  }

  void write_summary(const ProcSample& p0, const ProcSample& p1) {
    CsvSink csv;
    csv.open(ctx_.output_prefix + "_summary.csv");
    const ProcDelta pd = diff(p0, p1);
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
      s->summary(r);
      r.add("cpu_pct_avg", pd.cpu_pct, 2);
      r.add("rss_kb", p1.rss_kb);
      r.add("hwm_kb", p1.hwm_kb);
      r.add("invol_ctxsw", pd.invol_ctxsw);
      r.add("majflt", pd.majflt);
      csv.write(r);
      records.push_back(r);
      if (!cfg_.quiet) {
        std::string line = "RESUMEN";
        static const char* keys[] = {"stream", "received", "lost", "loss_pct", "msg_per_s", "mb_per_s", "e2e_mean_us",
                                     "e2e_std_us", "e2e_p50_us", "e2e_p95_us", "e2e_p99_us", "e2e_p999_us", "e2e_max_us",
                                     "e2e_corr_p50_us", "e2e_corr_p99_us", "deadline_missed", "deadline_miss_rate",
                                     "cpu_pct_avg", "rss_kb"};
        for (const auto& f : r.fields()) {
          for (const char* k : keys) {
            if (f.key == k) line += " " + f.key + "=" + f.value;
          }
        }
        log_info(cfg_, line);
      }
    }
    write_file_atomic(ctx_.output_prefix + "_summary.prom", to_prometheus(records, "dds_bench_summary_"));
    log_info(cfg_, "resultados en " + ctx_.output_prefix + "_*.csv");
  }

  BenchConfig cfg_;
  ProcessContext ctx_;
  std::unique_ptr<B> backend_;
  std::vector<std::unique_ptr<SubStreamBase<B>>> streams_;
  std::unique_ptr<typename B::WaitSet> ws_;
  PromHttpExporter prom_;
  CsvSink timeseries_;
  std::atomic<bool> reader_stop_{false};
};

}  // namespace bench
