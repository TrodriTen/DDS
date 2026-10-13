// SPDX-License-Identifier: Apache-2.0
// Data Reader (suscriptor) del benchmark sobre FastDdsBackend.
#include <iostream>

#include "fastdds_backend.hpp"
#include "bench/subscriber.hpp"

int main(int argc, char** argv) {
  bench::BenchConfig cfg;
  std::string error;
  if (!bench::parse_cli(argc, argv, "sub", "fastdds", cfg, error)) {
    std::cerr << "error: " << error << " (usa --help)" << std::endl;
    return 1;
  }
  bench::resolve_default_configs(cfg);
  return bench::SubscriberApp<bench::FastDdsBackend>(std::move(cfg)).run();
}
