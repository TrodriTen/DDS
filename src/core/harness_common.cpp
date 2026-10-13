// SPDX-License-Identifier: Apache-2.0
#include "bench/harness_common.hpp"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>

#include "bench/clock.hpp"

namespace bench {

std::atomic<bool>& stop_requested() {
  static std::atomic<bool> flag{false};
  return flag;
}

namespace {
void on_signal(int) { stop_requested().store(true); }

std::mutex& log_mutex() {
  static std::mutex m;
  return m;
}

void set_env_default(const char* name, const std::string& value) {
  if (!value.empty()) setenv(name, value.c_str(), 0);
}

std::string json_escape(const std::string& s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\t': o += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) continue;
        o += c;
    }
  }
  return o;
}
}  // namespace

void install_signal_handlers() {
  struct sigaction sa {};
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  signal(SIGPIPE, SIG_IGN);
}

void log_info(const BenchConfig& cfg, const std::string& msg) {
  if (cfg.quiet) return;
  std::lock_guard<std::mutex> lk(log_mutex());
  std::cout << "[" << cfg.role << "/" << cfg.middleware << "] " << msg << std::endl;
}

void log_warn(const std::string& msg) {
  std::lock_guard<std::mutex> lk(log_mutex());
  std::cerr << "[WARN] " << msg << std::endl;
}

ProcessContext prepare_process(BenchConfig& cfg) {
  ProcessContext ctx;
  ctx.env = rt::probe_environment();

  // Variables para la sustitución ${...} en los XML de Fast DDS y Cyclone.
  if (cfg.iface.empty()) {
    const char* e = std::getenv("DDS_BENCH_IFACE");
    cfg.iface = e ? e : default_route_interface();
  }
  setenv("DDS_BENCH_IFACE", cfg.iface.c_str(), 1);
  set_env_default("DDS_BENCH_IFACE_IP", interface_ipv4(cfg.iface));
  set_env_default("DDS_BENCH_MCAST_ADDR", cfg.mcast_addr);
  setenv("DDS_BENCH_DOMAIN", std::to_string(cfg.domain_id).c_str(), 1);
  // Política de los hilos internos del middleware (fastdds_profiles.xml / cyclonedds.xml)
  const bool fifo = cfg.rt.enable_fifo;
  const std::string mw_prio = std::to_string(fifo ? cfg.rt.middleware_priority : 0);
  setenv("DDS_BENCH_MW_POLICY", fifo ? "1" : "0", 1);
  setenv("DDS_BENCH_MW_PRIO", mw_prio.c_str(), 1);
  setenv("DDS_BENCH_MW_CLASS", fifo ? "realtime" : "default", 1);
  setenv("DDS_BENCH_IOX_PRIO", fifo ? ("FIFO:" + mw_prio).c_str() : "OTHER:0", 1);

  namespace fs = std::filesystem;
  const fs::path dir = fs::path(cfg.out_dir) / cfg.run_id;
  std::error_code ec;
  fs::create_directories(dir, ec);
  ctx.output_prefix = (dir / (cfg.role + "_" + cfg.middleware + "_n" + std::to_string(cfg.node_id))).string();

  if (!ctx.env.preempt_rt) ctx.warnings.emplace_back("el kernel NO es PREEMPT_RT: las latencias de cola no serán representativas");
  if (!ctx.env.ntp_synchronized) ctx.warnings.emplace_back("reloj no sincronizado (adjtimex): la E2E bruta incluye el offset; usar e2e_corr/RTT");

  std::string err;
  if (!rt::lock_process_memory(cfg.rt, &err)) ctx.warnings.push_back(err + " (¿ulimit -l / CAP_IPC_LOCK?)");
  if (cfg.rt.cpu_dma_latency_us >= 0) {
    ctx.dma_fd = rt::hold_cpu_dma_latency(cfg.rt.cpu_dma_latency_us);
    if (ctx.dma_fd < 0) ctx.warnings.emplace_back("no se pudo abrir /dev/cpu_dma_latency");
  }

  // El hilo principal crea las entidades DDS: los hilos del middleware heredan
  // su política y afinidad (PTHREAD_INHERIT_SCHED por defecto).
  err.clear();
  if (!rt::configure_current_thread(nullptr, cfg.rt.middleware_cpus, fifo ? cfg.rt.middleware_priority : 0,
                                    cfg.rt.prefault_stack_kb, &err)) {
    ctx.warnings.push_back("hilo principal/middleware: " + err);
  }
  for (const auto& w : ctx.warnings) log_warn(w);
  log_info(cfg, rt::describe(ctx.env));
  return ctx;
}

void enter_housekeeping(const BenchConfig& cfg) {
  std::string err;
  rt::configure_current_thread("bench.stats", cfg.rt.housekeeping_cpus, 0, 0, &err);
}

std::string read_phase(const std::string& path, const std::string& fallback) {
  if (path.empty()) return fallback;
  std::ifstream f(path);
  std::string line;
  if (!f || !std::getline(f, line)) return fallback;
  while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
  return line.empty() ? fallback : line;
}

HostSampler::HostSampler(std::string iface) : iface_(std::move(iface)) {
  proc_cur_ = read_proc_sample();
  udp_cur_ = read_udp_snmp();
  nic_cur_ = read_nic_stats(iface_);
}

void HostSampler::sample() {
  proc_prev_ = proc_cur_;
  udp_prev_ = udp_cur_;
  nic_prev_ = nic_cur_;
  proc_cur_ = read_proc_sample();
  udp_cur_ = read_udp_snmp();
  nic_cur_ = read_nic_stats(iface_);
  proc_delta_ = diff(proc_prev_, proc_cur_);
  window_s_ = static_cast<double>(proc_cur_.t_mono_ns - proc_prev_.t_mono_ns) / 1e9;
}

void HostSampler::append(Record& r) const {
  r.add("cpu_pct", proc_delta_.cpu_pct, 2);
  r.add("cpu_user_pct", proc_delta_.cpu_user_pct, 2);
  r.add("cpu_sys_pct", proc_delta_.cpu_sys_pct, 2);
  r.add("rss_kb", proc_delta_.rss_kb);
  r.add("hwm_kb", proc_delta_.hwm_kb);
  r.add("threads", proc_delta_.threads);
  r.add("vol_ctxsw", proc_delta_.vol_ctxsw);
  r.add("invol_ctxsw", proc_delta_.invol_ctxsw);
  r.add("minflt", proc_delta_.minflt);
  r.add("majflt", proc_delta_.majflt);
  r.add("udp_rcvbuf_err", udp_cur_.rcvbuf_errors - udp_prev_.rcvbuf_errors);
  r.add("udp_sndbuf_err", udp_cur_.sndbuf_errors - udp_prev_.sndbuf_errors);
  r.add("udp_in_err", udp_cur_.in_errors - udp_prev_.in_errors);
  r.add("nic_rx_drop", nic_cur_.rx_dropped - nic_prev_.rx_dropped);
  r.add("nic_tx_drop", nic_cur_.tx_dropped - nic_prev_.tx_dropped);
  const double w = window_s_ > 0 ? window_s_ : 1.0;
  r.add("nic_rx_mbps", static_cast<double>(nic_cur_.rx_bytes - nic_prev_.rx_bytes) * 8.0 / 1e6 / w, 3);
  r.add("nic_tx_mbps", static_cast<double>(nic_cur_.tx_bytes - nic_prev_.tx_bytes) * 8.0 / 1e6 / w, 3);
}

void add_identity(Record& r, const BenchConfig& cfg, const char* stream, const std::string& phase) {
  r.add("run_id", cfg.run_id, true);
  r.add("scenario", cfg.scenario, true);
  r.add("middleware", cfg.middleware, true);
  r.add("role", cfg.role, true);
  r.add("transport", cfg.transport, true);
  r.add("node", std::to_string(cfg.node_id), true);
  r.add("stream", stream, true);
  r.add("phase", phase, true);
}

double deadline_miss_rate(uint64_t missed, double elapsed_s, uint32_t instances, int64_t deadline_ns) {
  if (deadline_ns <= 0 || elapsed_s <= 0 || instances == 0) return 0.0;
  const double periods = static_cast<double>(instances) * elapsed_s * 1e9 / static_cast<double>(deadline_ns);
  return periods > 0 ? static_cast<double>(missed) / periods : 0.0;
}

void write_thread_report(const std::string& path, const BenchConfig& cfg) {
  CsvSink csv;
  if (!csv.open(path)) return;
  for (const auto& t : list_threads()) {
    Record r;
    r.add("run_id", cfg.run_id, true);
    r.add("middleware", cfg.middleware, true);
    r.add("role", cfg.role, true);
    r.add("tid", static_cast<int64_t>(t.tid));
    r.add("name", t.name, true);
    r.add("sched", t.sched, true);
    r.add("cpu_s", t.cpu_s, 3);
    csv.write(r);
    if (cfg.verbose) {
      log_info(cfg, "hilo " + std::to_string(t.tid) + " " + t.name + " " + t.sched + " cpu=" + std::to_string(t.cpu_s) + "s");
    }
  }
}

void write_meta_json(const std::string& path, const BenchConfig& cfg, const ProcessContext& ctx,
                     const std::string& middleware_version) {
  std::ofstream f(path);
  if (!f) return;
  const auto kv = [&](const char* k, const std::string& v, bool last = false) {
    f << "  \"" << k << "\": \"" << json_escape(v) << "\"" << (last ? "\n" : ",\n");
  };
  f << "{\n";
  kv("run_id", cfg.run_id);
  kv("scenario", cfg.scenario);
  kv("role", cfg.role);
  kv("middleware", cfg.middleware);
  kv("middleware_version", middleware_version);
  kv("transport", cfg.transport);
  kv("node_id", std::to_string(cfg.node_id));
  kv("domain_id", std::to_string(cfg.domain_id));
  kv("iface", cfg.iface);
  kv("kernel", ctx.env.kernel_release + " " + ctx.env.kernel_version);
  kv("preempt_rt", ctx.env.preempt_rt ? "true" : "false");
  kv("isolated_cpus", ctx.env.isolated_cpus);
  kv("clocksource", ctx.env.clocksource);
  kv("ntp_synchronized", ctx.env.ntp_synchronized ? "true" : "false");
  kv("ntp_esterror_us", std::to_string(ctx.env.ntp_esterror_us));
  kv("ntp_maxerror_us", std::to_string(ctx.env.ntp_maxerror_us));
  kv("fastdds_xml", cfg.fastdds_xml);
  kv("participant_profile", cfg.participant_profile);
  kv("cyclone_xml", cfg.cyclone_xml);
  kv("cyclone_qos_xml", cfg.cyclone_qos_xml);
  std::string warnings;
  for (const auto& w : ctx.warnings) warnings += w + "; ";
  kv("warnings", warnings);
  kv("config", cfg.describe(), true);
  f << "}\n";
}

size_t auto_raw_capacity(const BenchConfig& cfg, const StreamSpec& s, uint32_t divisor) {
  if (cfg.raw_capacity > 0) return cfg.raw_capacity;
  const double n = s.aggregate_rate_hz() * (cfg.duration_s + cfg.warmup_s + 10.0) * 1.2 / std::max(1u, divisor);
  return static_cast<size_t>(std::clamp(n, 1000.0, 20e6));
}

}  // namespace bench

namespace bench {

void resolve_default_configs(BenchConfig& cfg) {
  namespace fs = std::filesystem;
  std::vector<fs::path> dirs;
  if (const char* e = std::getenv("DDS_BENCH_CONFIG_DIR")) dirs.emplace_back(e);
  std::error_code ec;
  const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
  if (!ec) {
    const fs::path bin = exe.parent_path();
    dirs.push_back(bin / ".." / "share" / "dds_bench" / "config");
    dirs.push_back(bin / ".." / "config");
    dirs.push_back(bin / ".." / ".." / "config");
  }
  dirs.emplace_back("config");
  const auto find = [&](const char* name) -> std::string {
    for (const auto& d : dirs) {
      const fs::path p = d / name;
      if (fs::exists(p, ec)) return fs::weakly_canonical(p, ec).string();
    }
    return {};
  };
  if (cfg.middleware == "fastdds" && cfg.fastdds_xml.empty()) cfg.fastdds_xml = find("fastdds_profiles.xml");
  if (cfg.middleware == "cyclonedds") {
    if (cfg.cyclone_xml.empty()) {
      cfg.cyclone_xml = find("cyclonedds.xml");
      const char* overlay = cfg.transport == "udpm" ? "cyclonedds_multicast.xml"
                            : (cfg.transport == "shm" || cfg.transport == "zc") ? "cyclonedds_shm.xml"
                                                                                : nullptr;
      if (overlay != nullptr) {
        const std::string o = find(overlay);
        if (!o.empty()) cfg.cyclone_xml_overlay = o;
      }
    }
    if (cfg.cyclone_qos_xml.empty()) cfg.cyclone_qos_xml = find("cyclonedds_qos.xml");
  }
}

}  // namespace bench
