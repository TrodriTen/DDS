// SPDX-License-Identifier: Apache-2.0
#include "bench/metrics.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>

namespace bench {

Record& Record::add(std::string_view key, const std::string& v, bool label) {
  fields_.push_back({std::string(key), v, label, false});
  return *this;
}

Record& Record::add(std::string_view key, double v, int precision) {
  char buf[64];
  if (std::isfinite(v)) {
    std::snprintf(buf, sizeof(buf), "%.*f", precision, v);
  } else {
    std::snprintf(buf, sizeof(buf), "NaN");
  }
  fields_.push_back({std::string(key), buf, false, true});
  return *this;
}

Record& Record::add(std::string_view key, int64_t v) {
  fields_.push_back({std::string(key), std::to_string(v), false, true});
  return *this;
}

Record& Record::add(std::string_view key, uint64_t v) {
  fields_.push_back({std::string(key), std::to_string(v), false, true});
  return *this;
}

Record& Record::add_empty(std::string_view key) {
  fields_.push_back({std::string(key), "", false, true});
  return *this;
}

Record& Record::add_latency_us(std::string_view prefix, const LatencySummary& s, bool full) {
  const std::string p(prefix);
  const auto us = [](double ns) { return ns / 1e3; };
  if (s.count == 0) {
    for (const char* k : {"_mean_us", "_std_us", "_min_us", "_p50_us", "_p90_us", "_p95_us", "_p99_us",
                          "_p999_us", "_p9999_us", "_max_us"}) {
      if (!full && (std::strcmp(k, "_p90_us") == 0 || std::strcmp(k, "_p9999_us") == 0)) continue;
      add_empty(p + k);
    }
    return *this;
  }
  add(p + "_mean_us", us(s.mean));
  add(p + "_std_us", us(s.stddev));
  add(p + "_min_us", us(static_cast<double>(s.min)));
  add(p + "_p50_us", us(static_cast<double>(s.p50)));
  if (full) add(p + "_p90_us", us(static_cast<double>(s.p90)));
  add(p + "_p95_us", us(static_cast<double>(s.p95)));
  add(p + "_p99_us", us(static_cast<double>(s.p99)));
  add(p + "_p999_us", us(static_cast<double>(s.p999)));
  if (full) add(p + "_p9999_us", us(static_cast<double>(s.p9999)));
  add(p + "_max_us", us(static_cast<double>(s.max)));
  return *this;
}

bool CsvSink::open(const std::string& path) {
  path_ = path;
  out_.open(path, std::ios::out | std::ios::trunc);
  header_written_ = false;
  return static_cast<bool>(out_);
}

namespace {
std::string csv_escape(const std::string& v) {
  if (v.find_first_of(",\"\n") == std::string::npos) return v;
  std::string o = "\"";
  for (char c : v) {
    if (c == '"') o += '"';
    o += c;
  }
  return o + '"';
}
}  // namespace

void CsvSink::write(const Record& r) {
  if (!out_) return;
  if (!header_written_) {
    bool first = true;
    for (const auto& f : r.fields()) {
      out_ << (first ? "" : ",") << f.key;
      first = false;
    }
    out_ << '\n';
    header_written_ = true;
  }
  bool first = true;
  for (const auto& f : r.fields()) {
    out_ << (first ? "" : ",") << csv_escape(f.value);
    first = false;
  }
  out_ << '\n';
  out_.flush();
}

std::string to_prometheus(const std::vector<Record>& records, std::string_view prefix) {
  // Agrupa por nombre de métrica para emitir un único # TYPE por familia.
  std::map<std::string, std::vector<std::string>> families;
  for (const auto& r : records) {
    std::string labels;
    for (const auto& f : r.fields()) {
      if (!f.label) continue;
      std::string v;
      for (char c : f.value) {
        if (c == '"' || c == '\\') v += '\\';
        v += c;
      }
      labels += (labels.empty() ? "" : ",") + f.key + "=\"" + v + "\"";
    }
    for (const auto& f : r.fields()) {
      if (f.label || !f.numeric || f.value.empty()) continue;
      const std::string name = std::string(prefix) + f.key;
      families[name].push_back(name + "{" + labels + "} " + f.value);
    }
  }
  std::ostringstream os;
  for (const auto& [name, lines] : families) {
    os << "# TYPE " << name << " gauge\n";
    for (const auto& l : lines) os << l << '\n';
  }
  return os.str();
}

PromHttpExporter::~PromHttpExporter() { stop(); }

bool PromHttpExporter::start(int port, std::string* error) {
  listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listen_fd_ < 0) {
    if (error) *error = std::strerror(errno);
    return false;
  }
  const int one = 1;
  setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listen_fd_, 8) != 0) {
    if (error) *error = std::string("bind/listen :") + std::to_string(port) + ": " + std::strerror(errno);
    close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  running_ = true;
  thread_ = std::thread([this] { serve(); });
  return true;
}

void PromHttpExporter::stop() {
  if (!running_) return;
  running_ = false;
  shutdown(listen_fd_, SHUT_RDWR);
  close(listen_fd_);
  listen_fd_ = -1;
  if (thread_.joinable()) thread_.join();
}

void PromHttpExporter::set_text(std::string text) {
  std::lock_guard<std::mutex> lk(mu_);
  text_ = std::move(text);
}

void PromHttpExporter::serve() {
  pthread_setname_np(pthread_self(), "bench.prom");
  while (running_) {
    pollfd p{listen_fd_, POLLIN, 0};
    if (poll(&p, 1, 200) <= 0) continue;
    const int c = accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (c < 0) continue;
    timeval tv{1, 0};
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char req[1024];
    const ssize_t n = recv(c, req, sizeof(req) - 1, 0);
    std::string body;
    std::string status = "200 OK";
    if (n > 0) {
      req[n] = '\0';
      if (std::strncmp(req, "GET /metrics", 12) == 0 || std::strncmp(req, "GET / ", 6) == 0) {
        std::lock_guard<std::mutex> lk(mu_);
        body = text_;
      } else {
        status = "404 Not Found";
        body = "not found\n";
      }
    }
    std::string resp = "HTTP/1.1 " + status +
                       "\r\nContent-Type: text/plain; version=0.0.4; charset=utf-8\r\nContent-Length: " +
                       std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    size_t off = 0;
    while (off < resp.size()) {
      const ssize_t w = send(c, resp.data() + off, resp.size() - off, MSG_NOSIGNAL);
      if (w <= 0) break;
      off += static_cast<size_t>(w);
    }
    close(c);
  }
}

bool write_file_atomic(const std::string& path, const std::string& content) {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::out | std::ios::trunc);
    if (!f) return false;
    f << content;
  }
  return std::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace bench
