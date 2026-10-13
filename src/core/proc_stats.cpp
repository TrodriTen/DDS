// SPDX-License-Identifier: Apache-2.0
#include "bench/proc_stats.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <sstream>

#include "bench/clock.hpp"
#include "bench/rt.hpp"

namespace bench {
namespace {

// Campos de /proc/<pid>/stat tras el paréntesis de comm (que puede contener espacios).
bool parse_stat_line(const std::string& line, std::vector<std::string>& fields, std::string* comm) {
  const auto lp = line.find('(');
  const auto rp = line.rfind(')');
  if (lp == std::string::npos || rp == std::string::npos || rp < lp) return false;
  if (comm) *comm = line.substr(lp + 1, rp - lp - 1);
  std::istringstream is(line.substr(rp + 2));
  fields.clear();
  std::string tok;
  while (is >> tok) fields.push_back(tok);
  return true;
}

uint64_t read_u64(const std::string& path) {
  std::ifstream f(path);
  uint64_t v = 0;
  f >> v;
  return v;
}

}  // namespace

ProcSample read_proc_sample() {
  ProcSample s;
  s.t_mono_ns = now_mono_ns();
  {
    std::ifstream f("/proc/self/stat");
    std::string line;
    std::getline(f, line);
    std::vector<std::string> fl;
    // Tras "pid (comm) ": state(0) ppid(1) ... minflt(7) majflt(9) utime(11) stime(12) num_threads(17)
    if (parse_stat_line(line, fl, nullptr) && fl.size() > 17) {
      s.minflt = std::stoull(fl[7]);
      s.majflt = std::stoull(fl[9]);
      s.utime_ticks = std::stoull(fl[11]);
      s.stime_ticks = std::stoull(fl[12]);
      s.threads = static_cast<uint32_t>(std::stoul(fl[17]));
    }
  }
  {
    std::ifstream f("/proc/self/status");
    std::string key;
    while (f >> key) {
      if (key == "VmRSS:") f >> s.rss_kb;
      else if (key == "VmHWM:") f >> s.hwm_kb;
      else if (key == "voluntary_ctxt_switches:") f >> s.vol_ctxsw;
      else if (key == "nonvoluntary_ctxt_switches:") f >> s.invol_ctxsw;
      f.ignore(4096, '\n');
    }
  }
  return s;
}

ProcDelta diff(const ProcSample& prev, const ProcSample& cur) {
  ProcDelta d;
  static const double hz = static_cast<double>(sysconf(_SC_CLK_TCK));
  const double dt_s = static_cast<double>(cur.t_mono_ns - prev.t_mono_ns) / 1e9;
  if (dt_s > 0) {
    d.cpu_user_pct = 100.0 * static_cast<double>(cur.utime_ticks - prev.utime_ticks) / hz / dt_s;
    d.cpu_sys_pct = 100.0 * static_cast<double>(cur.stime_ticks - prev.stime_ticks) / hz / dt_s;
    d.cpu_pct = d.cpu_user_pct + d.cpu_sys_pct;
  }
  d.rss_kb = cur.rss_kb;
  d.hwm_kb = cur.hwm_kb;
  d.vol_ctxsw = cur.vol_ctxsw - prev.vol_ctxsw;
  d.invol_ctxsw = cur.invol_ctxsw - prev.invol_ctxsw;
  d.minflt = cur.minflt - prev.minflt;
  d.majflt = cur.majflt - prev.majflt;
  d.threads = cur.threads;
  return d;
}

UdpSnmp read_udp_snmp() {
  UdpSnmp u;
  std::ifstream f("/proc/net/snmp");
  std::string header, values;
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind("Udp:", 0) == 0) {
      if (header.empty()) header = line;
      else { values = line; break; }
    }
  }
  if (header.empty() || values.empty()) return u;
  std::istringstream hs(header), vs(values);
  std::string k, v;
  hs >> k;
  vs >> v;
  while (hs >> k && vs >> v) {
    const uint64_t n = std::stoull(v);
    if (k == "InDatagrams") u.in_datagrams = n;
    else if (k == "NoPorts") u.no_ports = n;
    else if (k == "InErrors") u.in_errors = n;
    else if (k == "OutDatagrams") u.out_datagrams = n;
    else if (k == "RcvbufErrors") u.rcvbuf_errors = n;
    else if (k == "SndbufErrors") u.sndbuf_errors = n;
    else if (k == "InCsumErrors") u.in_csum_errors = n;
  }
  return u;
}

NicStats read_nic_stats(const std::string& iface) {
  NicStats n;
  if (iface.empty()) return n;
  const std::string base = "/sys/class/net/" + iface + "/statistics/";
  n.rx_packets = read_u64(base + "rx_packets");
  n.tx_packets = read_u64(base + "tx_packets");
  n.rx_bytes = read_u64(base + "rx_bytes");
  n.tx_bytes = read_u64(base + "tx_bytes");
  n.rx_dropped = read_u64(base + "rx_dropped");
  n.tx_dropped = read_u64(base + "tx_dropped");
  n.rx_errors = read_u64(base + "rx_errors");
  n.tx_errors = read_u64(base + "tx_errors");
  return n;
}

std::string default_route_interface() {
  std::ifstream f("/proc/net/route");
  std::string line;
  std::getline(f, line);  // cabecera
  while (std::getline(f, line)) {
    std::istringstream is(line);
    std::string iface, dest;
    is >> iface >> dest;
    if (dest == "00000000") return iface;
  }
  return "lo";
}

std::string interface_ipv4(const std::string& iface) {
  ifaddrs* ifs = nullptr;
  if (getifaddrs(&ifs) != 0) return {};
  std::string out;
  for (ifaddrs* p = ifs; p != nullptr; p = p->ifa_next) {
    if (p->ifa_addr == nullptr || p->ifa_addr->sa_family != AF_INET) continue;
    if (iface != p->ifa_name) continue;
    char buf[INET_ADDRSTRLEN] = {};
    const auto* sin = reinterpret_cast<const sockaddr_in*>(p->ifa_addr);
    inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
    out = buf;
    break;
  }
  freeifaddrs(ifs);
  return out;
}

std::vector<ThreadInfo> list_threads() {
  std::vector<ThreadInfo> out;
  DIR* d = opendir("/proc/self/task");
  if (d == nullptr) return out;
  static const double hz = static_cast<double>(sysconf(_SC_CLK_TCK));
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    ThreadInfo ti;
    ti.tid = std::atoi(e->d_name);
    std::ifstream f(std::string("/proc/self/task/") + e->d_name + "/stat");
    std::string line;
    std::getline(f, line);
    std::vector<std::string> fl;
    if (parse_stat_line(line, fl, &ti.name) && fl.size() > 12) {
      ti.cpu_s = static_cast<double>(std::stoull(fl[11]) + std::stoull(fl[12])) / hz;
    }
    ti.sched = rt::thread_sched_string(ti.tid);
    out.push_back(std::move(ti));
  }
  closedir(d);
  return out;
}

}  // namespace bench
