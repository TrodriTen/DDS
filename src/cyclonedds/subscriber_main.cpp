// SPDX-License-Identifier: Apache-2.0
// Data Reader (suscriptor) del benchmark sobre CycloneBackend.
#include <iostream>

#include "cyclone_backend.hpp"
#include "bench/subscriber.hpp"

int main(int argc, char** argv) {
  bench::BenchConfig cfg;
  std::string error;
  if (!bench::parse_cli(argc, argv, "sub", "cyclonedds", cfg, error)) {
    std::cerr << "error: " << error << " (usa --help)" << std::endl;
    return 1;
  }
  bench::resolve_default_configs(cfg);
  return bench::SubscriberApp<bench::CycloneBackend>(std::move(cfg)).run();
}
