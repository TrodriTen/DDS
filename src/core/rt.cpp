// SPDX-License-Identifier: Apache-2.0
#include "bench/rt.hpp"

#include <alloca.h>
#include <fcntl.h>
#include <malloc.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/timex.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>

namespace bench::rt {
namespace {

std::string read_first_line(const char* path) {
  std::ifstream f(path);
  std::string line;
  if (f) std::getline(f, line);
  return line;
}

uint64_t rlimit_value(int resource) {
  rlimit rl{};
  if (getrlimit(resource, &rl) != 0) return 0;
  return rl.rlim_cur == RLIM_INFINITY ? std::numeric_limits<uint64_t>::max() : rl.rlim_cur;
}

std::string errno_string(const char* what) {
  return std::string(what) + ": " + std::strerror(errno);
}

}  // namespace

RtEnvironment probe_environment() {
  RtEnvironment env;
  utsname u{};
  if (uname(&u) == 0) {
    env.kernel_release = u.release;
    env.kernel_version = u.version;
  }
  env.preempt_rt = read_first_line("/sys/kernel/realtime") == "1" ||
                   env.kernel_version.find("PREEMPT_RT") != std::string::npos;
  env.isolated_cpus = read_first_line("/sys/devices/system/cpu/isolated");
  env.nohz_full = read_first_line("/sys/devices/system/cpu/nohz_full");
  const std::string rt_runtime = read_first_line("/proc/sys/kernel/sched_rt_runtime_us");
  env.sched_rt_runtime_us = rt_runtime.empty() ? 0 : std::stol(rt_runtime);
  env.cpu_governor = read_first_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
  env.clocksource = read_first_line("/sys/devices/system/clocksource/clocksource0/current_clocksource");
  env.rlimit_rtprio = rlimit_value(RLIMIT_RTPRIO);
  env.rlimit_memlock = rlimit_value(RLIMIT_MEMLOCK);

  timex tx{};
  const int state = adjtimex(&tx);
  if (state != -1) {
    env.ntp_synchronized = (state != TIME_ERROR) && !(tx.status & STA_UNSYNC);
    env.ntp_esterror_us = tx.esterror;
    env.ntp_maxerror_us = tx.maxerror;
    // tx.offset en µs salvo que STA_NANO esté activo
    env.ntp_offset_us = (tx.status & STA_NANO) ? tx.offset / 1000 : tx.offset;
  }
  return env;
}

std::string describe(const RtEnvironment& env) {
  std::ostringstream os;
  os << "kernel=" << env.kernel_release << " preempt_rt=" << (env.preempt_rt ? "yes" : "NO")
     << " isolated=[" << env.isolated_cpus << "] nohz_full=[" << env.nohz_full << "]"
     << " rt_runtime_us=" << env.sched_rt_runtime_us
     << " governor=" << (env.cpu_governor.empty() ? "n/a" : env.cpu_governor)
     << " clocksource=" << env.clocksource << " rlimit_rtprio=" << env.rlimit_rtprio
     << " ntp_sync=" << (env.ntp_synchronized ? "yes" : "no")
     << " ntp_esterror_us=" << env.ntp_esterror_us << " ntp_maxerror_us=" << env.ntp_maxerror_us;
  return os.str();
}

bool lock_process_memory(const RtOptions& opts, std::string* error) {
  // Nunca devolver memoria al SO ni usar mmap para bloques grandes: así el
  // heap que pre-tocamos aquí se reutiliza sin nuevos page-faults.
  mallopt(M_TRIM_THRESHOLD, -1);
  mallopt(M_MMAP_MAX, 0);
  mallopt(M_ARENA_MAX, 4);

  bool ok = true;
  if (opts.lock_memory) {
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
      if (error) *error = errno_string("mlockall(MCL_CURRENT|MCL_FUTURE)");
      ok = false;
    }
  }
  if (opts.prefault_heap_mb > 0) {
    const size_t bytes = opts.prefault_heap_mb * 1024 * 1024;
    auto* p = static_cast<volatile char*>(std::malloc(bytes));
    if (p != nullptr) {
      const long page = sysconf(_SC_PAGESIZE);
      for (size_t i = 0; i < bytes; i += static_cast<size_t>(page)) p[i] = 0;
      std::free(const_cast<char*>(p));
    }
  }
  return ok;
}

void prefault_stack(size_t kb) {
  if (kb == 0) return;
  const size_t bytes = kb * 1024;
  auto buf = static_cast<volatile unsigned char*>(alloca(bytes));
  const long page = sysconf(_SC_PAGESIZE);
  for (size_t i = 0; i < bytes; i += static_cast<size_t>(page)) buf[i] = 0;
}

bool configure_thread(pthread_t thread, const std::vector<int>& cpus, int priority,
                      std::string* error) {
  bool ok = true;
  if (!cpus.empty()) {
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int c : cpus) CPU_SET(c, &set);
    if (const int rc = pthread_setaffinity_np(thread, sizeof(set), &set); rc != 0) {
      if (error) *error += std::string("pthread_setaffinity_np: ") + std::strerror(rc) + "; ";
      ok = false;
    }
  }
  sched_param sp{};
  int policy = SCHED_OTHER;
  if (priority > 0) {
    policy = SCHED_FIFO;
    sp.sched_priority = priority;
  }
  if (const int rc = pthread_setschedparam(thread, policy, &sp); rc != 0) {
    if (error) {
      *error += std::string("pthread_setschedparam(") + (policy == SCHED_FIFO ? "SCHED_FIFO" : "SCHED_OTHER") +
                "," + std::to_string(priority) + "): " + std::strerror(rc) +
                " (¿CAP_SYS_NICE / RLIMIT_RTPRIO?); ";
    }
    ok = false;
  }
  return ok;
}

bool configure_current_thread(const char* name, const std::vector<int>& cpus, int priority,
                              size_t prefault_stack_kb, std::string* error) {
  if (name != nullptr) pthread_setname_np(pthread_self(), name);
  // Los hilos SCHED_OTHER usan timer slack de 50 µs por defecto; lo reducimos.
  prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);
  const bool ok = configure_thread(pthread_self(), cpus, priority, error);
  prefault_stack(prefault_stack_kb);
  return ok;
}

int hold_cpu_dma_latency(int32_t microseconds) {
  const int fd = open("/dev/cpu_dma_latency", O_WRONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  if (write(fd, &microseconds, sizeof(microseconds)) != static_cast<ssize_t>(sizeof(microseconds))) {
    close(fd);
    return -1;
  }
  return fd;  // se mantiene abierto mientras viva el proceso
}

std::vector<int> parse_cpu_list(const std::string& s) {
  std::vector<int> cpus;
  std::stringstream ss(s);
  std::string tok;
  while (std::getline(ss, tok, ',')) {
    if (tok.empty()) continue;
    const auto dash = tok.find('-');
    if (dash == std::string::npos) {
      cpus.push_back(std::stoi(tok));
    } else {
      const int a = std::stoi(tok.substr(0, dash));
      const int b = std::stoi(tok.substr(dash + 1));
      for (int c = a; c <= b; ++c) cpus.push_back(c);
    }
  }
  return cpus;
}

std::string format_cpu_list(const std::vector<int>& cpus) {
  std::string out;
  for (size_t i = 0; i < cpus.size(); ++i) {
    if (i) out += ',';
    out += std::to_string(cpus[i]);
  }
  return out;
}

std::string thread_sched_string(int tid) {
  sched_param sp{};
  const int policy = sched_getscheduler(tid);
  sched_getparam(tid, &sp);
  const char* name = "OTHER";
  switch (policy) {
    case SCHED_FIFO: name = "FIFO"; break;
    case SCHED_RR: name = "RR"; break;
    case SCHED_BATCH: name = "BATCH"; break;
    case SCHED_IDLE: name = "IDLE"; break;
    default: break;
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  std::string cpus;
  if (sched_getaffinity(tid, sizeof(set), &set) == 0) {
    for (int c = 0; c < CPU_SETSIZE; ++c) {
      if (CPU_ISSET(c, &set)) {
        if (!cpus.empty()) cpus += ',';
        cpus += std::to_string(c);
      }
    }
  }
  return std::string(name) + ":" + std::to_string(sp.sched_priority) + " cpus=" + cpus;
}

}  // namespace bench::rt
