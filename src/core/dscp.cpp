// SPDX-License-Identifier: Apache-2.0
#include "bench/dscp.hpp"

#include <dirent.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace bench {

DscpMapping map_transport_priority(int32_t p) noexcept {
  if (p <= 0) return {0, 0, "BE"};
  if (p < 10) return {10, 1, "AF11"};
  if (p < 20) return {18, 2, "AF21"};
  if (p < 30) return {26, 3, "AF31"};
  if (p < 40) return {34, 4, "AF41"};
  if (p < 50) return {40, 5, "CS5"};
  return {46, 6, "EF"};
}

int parse_dscp(const std::string& s) {
  std::string u = s;
  std::transform(u.begin(), u.end(), u.begin(), [](unsigned char c) { return std::toupper(c); });
  if (u.empty() || u == "AUTO") return -1;
  if (u == "OFF" || u == "NONE") return -2;
  if (u == "BE" || u == "CS0") return 0;
  if (u == "EF") return 46;
  if (u == "VA") return 44;
  if (u.size() == 3 && u.rfind("CS", 0) == 0 && u[2] >= '1' && u[2] <= '7') return (u[2] - '0') * 8;
  if (u.size() == 4 && u.rfind("AF", 0) == 0) {
    const int cls = u[2] - '0';
    const int drop = u[3] - '0';
    if (cls >= 1 && cls <= 4 && drop >= 1 && drop <= 3) return cls * 8 + drop * 2;
  }
  const int v = std::atoi(s.c_str());
  return std::clamp(v, 0, 63);
}

TagResult tag_process_udp_sockets(int dscp, int so_priority) {
  TagResult r;
  DIR* d = opendir("/proc/self/fd");
  if (d == nullptr) {
    r.last_error = std::strerror(errno);
    return r;
  }
  const int tos = (dscp & 0x3f) << 2;  // ECN (2 bits bajos) a 0
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    const int fd = std::atoi(e->d_name);
    struct stat st {};
    if (fstat(fd, &st) != 0 || !S_ISSOCK(st.st_mode)) continue;
    int type = 0, domain = 0;
    socklen_t len = sizeof(type);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) != 0 || type != SOCK_DGRAM) continue;
    len = sizeof(domain);
    if (getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &domain, &len) != 0) continue;
    int rc = -1;
    if (domain == AF_INET) {
      rc = setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
    } else if (domain == AF_INET6) {
      rc = setsockopt(fd, IPPROTO_IPV6, IPV6_TCLASS, &tos, sizeof(tos));
    } else {
      continue;  // AF_UNIX (p.ej. SHM/iceoryx) no aplica
    }
    if (rc == 0 && so_priority >= 0) {
      rc = setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &so_priority, sizeof(so_priority));
    }
    if (rc == 0) {
      ++r.sockets_tagged;
    } else {
      ++r.sockets_failed;
      r.last_error = std::strerror(errno);
    }
  }
  closedir(d);
  return r;
}

}  // namespace bench
