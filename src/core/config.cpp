// SPDX-License-Identifier: Apache-2.0
#include "bench/config.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>

#include "bench/clock.hpp"
#include "bench/dscp.hpp"

namespace bench {

const char* to_string(SensorKind k) noexcept {
  switch (k) {
    case SensorKind::Radar: return "radar";
    case SensorKind::Sonar: return "sonar";
    case SensorKind::Weather: return "weather";
  }
  return "?";
}
const char* to_string(Reliability r) noexcept { return r == Reliability::Reliable ? "RELIABLE" : "BEST_EFFORT"; }
const char* to_string(HistoryKind h) noexcept { return h == HistoryKind::KeepLast ? "KEEP_LAST" : "KEEP_ALL"; }
const char* to_string(LivelinessKind l) noexcept {
  switch (l) {
    case LivelinessKind::Automatic: return "AUTOMATIC";
    case LivelinessKind::ManualByParticipant: return "MANUAL_BY_PARTICIPANT";
    case LivelinessKind::ManualByTopic: return "MANUAL_BY_TOPIC";
  }
  return "?";
}
const char* to_string(DataRep d) noexcept {
  switch (d) {
    case DataRep::Default: return "default";
    case DataRep::Xcdr1: return "XCDR1";
    case DataRep::Xcdr2: return "XCDR2";
  }
  return "?";
}

std::optional<SensorKind> parse_sensor(std::string_view s) noexcept {
  if (s == "radar") return SensorKind::Radar;
  if (s == "sonar") return SensorKind::Sonar;
  if (s == "weather" || s == "meteo") return SensorKind::Weather;
  return std::nullopt;
}

void QosSpec::merge_from(const QosSpec& o) {
#define BENCH_MERGE(f) \
  if (o.f) f = o.f
  BENCH_MERGE(reliability);
  BENCH_MERGE(max_blocking_ns);
  BENCH_MERGE(history);
  BENCH_MERGE(depth);
  BENCH_MERGE(max_samples);
  BENCH_MERGE(max_instances);
  BENCH_MERGE(max_samples_per_instance);
  BENCH_MERGE(deadline_ns);
  BENCH_MERGE(latency_budget_ns);
  BENCH_MERGE(liveliness);
  BENCH_MERGE(lease_ns);
  BENCH_MERGE(transport_priority);
  BENCH_MERGE(data_rep);
#undef BENCH_MERGE
}

std::string QosSpec::describe() const {
  std::ostringstream os;
  auto ms = [](int64_t ns) { return ns == 0 ? std::string("inf") : std::to_string(static_cast<double>(ns) / 1e6) + "ms"; };
  if (reliability) os << to_string(*reliability) << ' ';
  if (history) os << to_string(*history);
  if (depth) os << '(' << *depth << ')';
  os << ' ';
  if (max_samples || max_instances || max_samples_per_instance) {
    os << "RL[" << max_samples.value_or(-1) << '/' << max_instances.value_or(-1) << '/'
       << max_samples_per_instance.value_or(-1) << "] ";
  }
  if (deadline_ns) os << "deadline=" << ms(*deadline_ns) << ' ';
  if (latency_budget_ns) os << "latency_budget=" << ms(*latency_budget_ns) << ' ';
  if (liveliness) os << "liveliness=" << to_string(*liveliness) << ' ';
  if (lease_ns) os << "lease=" << ms(*lease_ns) << ' ';
  if (transport_priority) os << "transport_priority=" << *transport_priority << ' ';
  if (data_rep) os << "data_rep=" << to_string(*data_rep);
  return os.str();
}

int64_t StreamSpec::period_ns() const noexcept {
  const double agg = aggregate_rate_hz();
  return agg > 0 ? static_cast<int64_t>(std::llround(1e9 / agg)) : kNsPerSec;
}

StreamSpec default_stream(SensorKind k) {
  StreamSpec s;
  s.kind = k;
  QosSpec& q = s.qos;
  q.reliability = Reliability::Reliable;
  q.max_blocking_ns = 100 * kNsPerMs;
  q.history = HistoryKind::KeepLast;
  q.latency_budget_ns = 0;
  q.liveliness = LivelinessKind::Automatic;
  q.lease_ns = 1 * kNsPerSec;
  q.data_rep = DataRep::Default;
  switch (k) {
    case SensorKind::Radar:
      // 16 pistas x 100 Hz = 1600 msg/s; periodo por pista 10 ms, DEADLINE 20 ms (2x margen)
      s.rate_hz = 100.0;
      s.instances = 16;
      q.depth = 8;
      q.max_instances = 64;
      q.max_samples_per_instance = 8;
      q.max_samples = 64 * 8;
      q.deadline_ns = 20 * kNsPerMs;
      q.transport_priority = 50;  // -> EF
      break;
    case SensorKind::Sonar:
      // 4 transductores x 50 Hz = 200 msg/s, 256..1024 B de muestras crudas
      s.rate_hz = 50.0;
      s.instances = 4;
      q.depth = 4;
      q.max_instances = 8;
      q.max_samples_per_instance = 4;
      q.max_samples = 8 * 4;
      q.deadline_ns = 50 * kNsPerMs;
      q.transport_priority = 30;  // -> AF41
      break;
    case SensorKind::Weather:
      // 4 estaciones x 1 Hz; DEADLINE 1.5 s
      s.rate_hz = 1.0;
      s.instances = 4;
      q.depth = 1;
      q.max_instances = 8;
      q.max_samples_per_instance = 1;
      q.max_samples = 8;
      q.deadline_ns = 1500 * kNsPerMs;
      q.transport_priority = 10;  // -> AF21
      break;
  }
  return s;
}

std::string BenchConfig::describe() const {
  std::ostringstream os;
  os << "role=" << role << " middleware=" << middleware << " run_id=" << run_id << " scenario=" << scenario
     << " transport=" << transport << " domain=" << domain_id << " node=" << node_id << " duration=" << duration_s
     << "s warmup=" << warmup_s << "s echo_every=" << echo_every << " loans=" << (loans ? "on" : "off")
     << " iface=" << iface << "\n";
  for (const auto& s : streams) {
    os << "  stream " << to_string(s.kind) << " topic=" << s.topic << " rate=" << s.rate_hz << "Hz x "
       << s.instances << " inst (" << s.aggregate_rate_hz() << " msg/s)"
       << (s.qos_profile.empty() ? "" : " profile=" + s.qos_profile) << " qos: " << s.qos.describe() << "\n";
  }
  os << "  rt: fifo=" << (rt.enable_fifo ? "on" : "off") << " app_prio=" << rt.app_priority
     << " mw_prio=" << rt.middleware_priority << " app_cpus=[" << rt::format_cpu_list(rt.app_cpus)
     << "] mw_cpus=[" << rt::format_cpu_list(rt.middleware_cpus) << "] mlock=" << (rt.lock_memory ? "on" : "off");
  return os.str();
}

// ----------------------------------------------------------------------------
namespace {

const char* kHelp = R"(Uso: %s [opciones]

Identificación
  --run-id ID                 Identificador de la ejecución (por defecto: fecha-hora)
  --scenario NOMBRE           Etiqueta libre del escenario (CSV/Prometheus)
  --node-id N                 Identificador numérico del nodo (por defecto 1)
  --domain N                  Dominio DDS (por defecto 42)
  --transport udp|udpm|shm|zc Transporte (Fast DDS: elige perfil de participante;
                              Cyclone: etiqueta, el transporte lo fija --cyclone-xml)
  --topic-prefix P            Prefijo de los topics (por defecto "bench_")

Flujos de sensores
  --streams radar,sonar,weather
  --rate S=HZ[,S=HZ]          Frecuencia por instancia (p.ej. radar=100)
  --instances S=N[,...]       Nº de instancias/keys (pistas, transductores, estaciones)
  --sonar-bytes MIN:MAX       Tamaño de la secuencia de muestras sonar (<=1024)

QoS (valor global o por flujo: "20" o "radar=20,sonar=50")
  --reliability reliable|best_effort
  --max-blocking-ms MS
  --history keep_last|keep_all   --depth N
  --max-samples N  --max-instances N  --max-samples-per-instance N   (-1 = ilimitado)
  --deadline-ms MS            (0 = infinito)
  --latency-budget-us US
  --liveliness automatic|manual_participant|manual_topic  --lease-ms MS
  --transport-priority N
  --data-rep default|xcdr1|xcdr2
  --qos-profile NOMBRE        Perfil XML como base de QoS (sólo se aplican los
                              campos indicados explícitamente por CLI)
  --loans                     Zero-copy (loans) si tipo/transporte lo permiten

Configuración del middleware
  --fastdds-xml FICHERO       Perfiles XML de Fast DDS
  --participant-profile NOMBRE   (por defecto participant_<transport>)
  --fastdds-memory-policy preallocated|preallocated_realloc|dynamic|dynamic_reusable
  --cyclone-xml FICHERO       Configuración de dominio Cyclone (CYCLONEDDS_URI)
  --cyclone-qos-xml FICHERO   QoS Provider OMG DDS-XML de Cyclone
  --iface IFACE               Interfaz de red de pruebas (exporta DDS_BENCH_IFACE)
  --mcast-addr A.B.C.D        Grupo multicast para datos (DDS_BENCH_MCAST_ADDR)

Medida
  --duration S  --warmup S  --drain S
  --match-timeout S  --expected-matches N
  --window-ms MS              Ventana de agregación de métricas (por defecto 1000)
  --echo-every N              [pub] Pide eco (RTT) cada N muestras (0 = desactivado)
  --no-echo                   [sub] No responder ecos
  --raw-capacity N            Muestras crudas por flujo para percentiles exactos
  --seed N

Tiempo real
  --no-rt                     Sin SCHED_FIFO
  --prio N  --echo-prio N  --mw-prio N
  --cpus LISTA  --mw-cpus LISTA  --hk-cpus LISTA     (p.ej. 2,3 o 2-3)
  --no-mlock  --prefault-heap-mb N  --prefault-stack-kb N
  --cpu-dma-latency US        Mantiene /dev/cpu_dma_latency (0 = sin C-states)

DSCP
  --dscp auto|off|EF|AF41|CS5|0-63   (auto: derivado de TRANSPORT_PRIORITY)
  --so-priority N

Salida
  --out-dir DIR  --prom-port PUERTO  --prom-textfile FICHERO  --phase-file FICHERO
  -v|--verbose  -q|--quiet  -h|--help
)";

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string tok;
  while (std::getline(ss, tok, sep)) {
    if (!tok.empty()) out.push_back(tok);
  }
  return out;
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

struct CliError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// "20" -> {"*":"20"} ; "radar=20,sonar=50" -> {...}
std::map<std::string, std::string> per_stream_values(const std::string& v) {
  std::map<std::string, std::string> m;
  if (v.find('=') == std::string::npos) {
    m["*"] = v;
    return m;
  }
  for (const auto& kv : split(v, ',')) {
    const auto eq = kv.find('=');
    if (eq == std::string::npos) throw CliError("valor por flujo inválido: " + kv);
    const std::string key = lower(kv.substr(0, eq));
    if (!parse_sensor(key)) throw CliError("sensor desconocido: " + key);
    m[key] = kv.substr(eq + 1);
  }
  return m;
}

double to_double(const std::string& v) {
  char* end = nullptr;
  const double d = std::strtod(v.c_str(), &end);
  if (end == v.c_str()) throw CliError("número inválido: " + v);
  return d;
}

int64_t to_int(const std::string& v) {
  char* end = nullptr;
  const long long n = std::strtoll(v.c_str(), &end, 10);
  if (end == v.c_str()) throw CliError("entero inválido: " + v);
  return n;
}

std::string default_run_id() {
  const std::time_t t = std::time(nullptr);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", std::localtime(&t));
  return buf;
}

}  // namespace

bool parse_cli(int argc, char** argv, const std::string& role, const std::string& middleware,
               BenchConfig& cfg, std::string& error) {
  cfg.role = role;
  cfg.middleware = middleware;
  cfg.program = argc > 0 ? argv[0] : "bench";

  // QoS indicada explícitamente por CLI (global "*" o por sensor)
  std::map<std::string, QosSpec> cli_qos;
  std::map<std::string, std::string> qos_profile;
  std::map<std::string, double> rate;
  std::map<std::string, uint32_t> instances;
  std::vector<std::string> stream_names = {"radar", "sonar", "weather"};
  uint32_t sonar_min = 256, sonar_max = 1024;

  using Handler = std::function<void(const std::string&)>;
  auto qos_opt = [&](auto setter) -> Handler {
    return [&cli_qos, setter](const std::string& v) {
      for (const auto& [k, val] : per_stream_values(v)) setter(cli_qos[k], val);
    };
  };

  const std::map<std::string, Handler> with_value = {
      {"--run-id", [&](const std::string& v) { cfg.run_id = v; }},
      {"--scenario", [&](const std::string& v) { cfg.scenario = v; }},
      {"--node-id", [&](const std::string& v) { cfg.node_id = static_cast<uint32_t>(to_int(v)); }},
      {"--domain", [&](const std::string& v) { cfg.domain_id = static_cast<int>(to_int(v)); }},
      {"--transport", [&](const std::string& v) { cfg.transport = lower(v); }},
      {"--topic-prefix", [&](const std::string& v) { cfg.topic_prefix = v; }},
      {"--streams", [&](const std::string& v) { stream_names = split(lower(v), ','); }},
      {"--rate", [&](const std::string& v) {
         for (const auto& [k, val] : per_stream_values(v)) rate[k] = to_double(val);
       }},
      {"--instances", [&](const std::string& v) {
         for (const auto& [k, val] : per_stream_values(v)) instances[k] = static_cast<uint32_t>(to_int(val));
       }},
      {"--sonar-bytes", [&](const std::string& v) {
         const auto parts = split(v, ':');
         if (parts.size() != 2) throw CliError("--sonar-bytes espera MIN:MAX");
         sonar_min = static_cast<uint32_t>(to_int(parts[0]));
         sonar_max = static_cast<uint32_t>(to_int(parts[1]));
         if (sonar_min > sonar_max || sonar_max > 1024) throw CliError("--sonar-bytes fuera de rango (<=1024)");
       }},
      {"--reliability", qos_opt([](QosSpec& q, const std::string& v) {
         const std::string l = lower(v);
         if (l == "reliable") q.reliability = Reliability::Reliable;
         else if (l == "best_effort" || l == "besteffort" || l == "be") q.reliability = Reliability::BestEffort;
         else throw CliError("--reliability inválida: " + v);
       })},
      {"--max-blocking-ms", qos_opt([](QosSpec& q, const std::string& v) {
         q.max_blocking_ns = static_cast<int64_t>(to_double(v) * kNsPerMs);
       })},
      {"--history", qos_opt([](QosSpec& q, const std::string& v) {
         const std::string l = lower(v);
         if (l == "keep_last") q.history = HistoryKind::KeepLast;
         else if (l == "keep_all") q.history = HistoryKind::KeepAll;
         else throw CliError("--history inválida: " + v);
       })},
      {"--depth", qos_opt([](QosSpec& q, const std::string& v) { q.depth = static_cast<int32_t>(to_int(v)); })},
      {"--max-samples", qos_opt([](QosSpec& q, const std::string& v) { q.max_samples = static_cast<int32_t>(to_int(v)); })},
      {"--max-instances", qos_opt([](QosSpec& q, const std::string& v) { q.max_instances = static_cast<int32_t>(to_int(v)); })},
      {"--max-samples-per-instance", qos_opt([](QosSpec& q, const std::string& v) {
         q.max_samples_per_instance = static_cast<int32_t>(to_int(v));
       })},
      {"--deadline-ms", qos_opt([](QosSpec& q, const std::string& v) {
         q.deadline_ns = static_cast<int64_t>(to_double(v) * kNsPerMs);
       })},
      {"--latency-budget-us", qos_opt([](QosSpec& q, const std::string& v) {
         q.latency_budget_ns = static_cast<int64_t>(to_double(v) * kNsPerUs);
       })},
      {"--liveliness", qos_opt([](QosSpec& q, const std::string& v) {
         const std::string l = lower(v);
         if (l == "automatic") q.liveliness = LivelinessKind::Automatic;
         else if (l == "manual_participant") q.liveliness = LivelinessKind::ManualByParticipant;
         else if (l == "manual_topic") q.liveliness = LivelinessKind::ManualByTopic;
         else throw CliError("--liveliness inválida: " + v);
       })},
      {"--lease-ms", qos_opt([](QosSpec& q, const std::string& v) {
         q.lease_ns = static_cast<int64_t>(to_double(v) * kNsPerMs);
       })},
      {"--transport-priority", qos_opt([](QosSpec& q, const std::string& v) {
         q.transport_priority = static_cast<int32_t>(to_int(v));
       })},
      {"--data-rep", qos_opt([](QosSpec& q, const std::string& v) {
         const std::string l = lower(v);
         if (l == "default") q.data_rep = DataRep::Default;
         else if (l == "xcdr1" || l == "xcdr") q.data_rep = DataRep::Xcdr1;
         else if (l == "xcdr2") q.data_rep = DataRep::Xcdr2;
         else throw CliError("--data-rep inválida: " + v);
       })},
      {"--qos-profile", [&](const std::string& v) {
         for (const auto& [k, val] : per_stream_values(v)) qos_profile[k] = val;
       }},
      {"--fastdds-xml", [&](const std::string& v) { cfg.fastdds_xml = v; }},
      {"--participant-profile", [&](const std::string& v) { cfg.participant_profile = v; }},
      {"--fastdds-memory-policy", [&](const std::string& v) { cfg.fastdds_memory_policy = lower(v); }},
      {"--cyclone-xml", [&](const std::string& v) { cfg.cyclone_xml = v; }},
      {"--cyclone-qos-xml", [&](const std::string& v) { cfg.cyclone_qos_xml = v; }},
      {"--iface", [&](const std::string& v) { cfg.iface = v; }},
      {"--mcast-addr", [&](const std::string& v) { cfg.mcast_addr = v; }},
      {"--duration", [&](const std::string& v) { cfg.duration_s = to_double(v); }},
      {"--warmup", [&](const std::string& v) { cfg.warmup_s = to_double(v); }},
      {"--drain", [&](const std::string& v) { cfg.drain_s = to_double(v); }},
      {"--match-timeout", [&](const std::string& v) { cfg.match_timeout_s = to_double(v); }},
      {"--expected-matches", [&](const std::string& v) { cfg.expected_matches = static_cast<int>(to_int(v)); }},
      {"--window-ms", [&](const std::string& v) { cfg.window_ns = static_cast<int64_t>(to_double(v) * kNsPerMs); }},
      {"--echo-every", [&](const std::string& v) { cfg.echo_every = static_cast<uint32_t>(to_int(v)); }},
      {"--raw-capacity", [&](const std::string& v) { cfg.raw_capacity = static_cast<size_t>(to_int(v)); }},
      {"--seed", [&](const std::string& v) { cfg.seed = static_cast<uint64_t>(to_int(v)); }},
      {"--prio", [&](const std::string& v) { cfg.rt.app_priority = static_cast<int>(to_int(v)); }},
      {"--echo-prio", [&](const std::string& v) { cfg.rt.echo_priority = static_cast<int>(to_int(v)); }},
      {"--mw-prio", [&](const std::string& v) { cfg.rt.middleware_priority = static_cast<int>(to_int(v)); }},
      {"--cpus", [&](const std::string& v) { cfg.rt.app_cpus = rt::parse_cpu_list(v); }},
      {"--mw-cpus", [&](const std::string& v) { cfg.rt.middleware_cpus = rt::parse_cpu_list(v); }},
      {"--hk-cpus", [&](const std::string& v) { cfg.rt.housekeeping_cpus = rt::parse_cpu_list(v); }},
      {"--prefault-heap-mb", [&](const std::string& v) { cfg.rt.prefault_heap_mb = static_cast<size_t>(to_int(v)); }},
      {"--prefault-stack-kb", [&](const std::string& v) { cfg.rt.prefault_stack_kb = static_cast<size_t>(to_int(v)); }},
      {"--cpu-dma-latency", [&](const std::string& v) { cfg.rt.cpu_dma_latency_us = static_cast<int>(to_int(v)); }},
      {"--dscp", [&](const std::string& v) { cfg.dscp = parse_dscp(v); }},
      {"--so-priority", [&](const std::string& v) { cfg.so_priority = static_cast<int>(to_int(v)); }},
      {"--out-dir", [&](const std::string& v) { cfg.out_dir = v; }},
      {"--prom-port", [&](const std::string& v) { cfg.prom_port = static_cast<int>(to_int(v)); }},
      {"--prom-textfile", [&](const std::string& v) { cfg.prom_textfile = v; }},
      {"--phase-file", [&](const std::string& v) { cfg.phase_file = v; }},
  };
  const std::map<std::string, std::function<void()>> flags = {
      {"--loans", [&] { cfg.loans = true; }},
      {"--no-echo", [&] { cfg.echo_enabled = false; }},
      {"--no-rt", [&] { cfg.rt.enable_fifo = false; }},
      {"--no-mlock", [&] { cfg.rt.lock_memory = false; }},
      {"-v", [&] { cfg.verbose = true; }},
      {"--verbose", [&] { cfg.verbose = true; }},
      {"-q", [&] { cfg.quiet = true; }},
      {"--quiet", [&] { cfg.quiet = true; }},
  };

  try {
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      if (arg == "-h" || arg == "--help") {
        std::printf(kHelp, cfg.program.c_str());
        std::exit(0);
      }
      std::string value;
      bool has_inline = false;
      if (const auto eq = arg.find('='); eq != std::string::npos && arg.rfind("--", 0) == 0) {
        value = arg.substr(eq + 1);
        arg = arg.substr(0, eq);
        has_inline = true;
      }
      if (auto f = flags.find(arg); f != flags.end()) {
        f->second();
        continue;
      }
      auto h = with_value.find(arg);
      if (h == with_value.end()) throw CliError("opción desconocida: " + arg);
      if (!has_inline) {
        if (i + 1 >= argc) throw CliError("falta valor para " + arg);
        value = argv[++i];
      }
      h->second(value);
    }

    if (cfg.run_id.empty()) cfg.run_id = default_run_id();
    if (cfg.participant_profile.empty()) cfg.participant_profile = "participant_" + cfg.transport;

    // Construcción de los flujos: preset -> (perfil XML) -> overrides CLI
    cfg.streams.clear();
    for (const auto& name : stream_names) {
      const auto kind = parse_sensor(name);
      if (!kind) throw CliError("sensor desconocido en --streams: " + name);
      StreamSpec s = default_stream(*kind);
      const auto pick = [&](const auto& m) { return m.count(name) ? m.find(name) : m.find("*"); };
      if (auto it = pick(rate); it != rate.end()) s.rate_hz = it->second;
      if (auto it = pick(instances); it != instances.end()) s.instances = std::max(1u, it->second);
      if (auto it = pick(qos_profile); it != qos_profile.end()) s.qos_profile = it->second;
      if (!s.qos_profile.empty()) s.qos = QosSpec{};  // la base la pone el XML
      if (auto it = cli_qos.find("*"); it != cli_qos.end()) s.qos.merge_from(it->second);
      if (auto it = cli_qos.find(name); it != cli_qos.end()) s.qos.merge_from(it->second);
      s.sonar_min_bytes = sonar_min;
      s.sonar_max_bytes = sonar_max;
      s.stream_id = (cfg.node_id << 8) | static_cast<uint32_t>(*kind);
      switch (*kind) {
        case SensorKind::Radar: s.topic = cfg.topic_prefix + "RadarTrack"; break;
        case SensorKind::Sonar: s.topic = cfg.topic_prefix + "SonarPing"; break;
        case SensorKind::Weather: s.topic = cfg.topic_prefix + "WeatherStation"; break;
      }
      s.echo_topic = s.topic + "_Echo";
      // Coherencia de ResourceLimits con el nº de instancias del preset
      if (s.qos_profile.empty() && s.qos.max_instances && *s.qos.max_instances >= 0 &&
          static_cast<uint32_t>(*s.qos.max_instances) < s.instances) {
        s.qos.max_instances = static_cast<int32_t>(s.instances);
        if (s.qos.max_samples && *s.qos.max_samples >= 0 && s.qos.max_samples_per_instance) {
          s.qos.max_samples = std::max(*s.qos.max_samples,
                                       static_cast<int32_t>(s.instances) * *s.qos.max_samples_per_instance);
        }
      }
      cfg.streams.push_back(std::move(s));
    }
    if (cfg.streams.empty()) throw CliError("no hay flujos (--streams)");
  } catch (const std::exception& e) {
    error = e.what();
    return false;
  }
  return true;
}

}  // namespace bench
