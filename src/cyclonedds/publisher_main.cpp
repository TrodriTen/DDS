// SPDX-License-Identifier: Apache-2.0
// Data Writer (publicador) del benchmark sobre CycloneBackend.
#include <iostream>

#include "cyclone_backend.hpp"
#include "bench/publisher.hpp"

int main(int argc, char** argv) {
  bench::BenchConfig cfg;
  std::string error;
  if (!bench::parse_cli(argc, argv, "pub", "cyclonedds", cfg, error)) {
    std::cerr << "error: " << error << " (usa --help)" << std::endl;
    return 1;
  }
  bench::resolve_default_configs(cfg);
  return bench::PublisherApp<bench::CycloneBackend>(std::move(cfg)).run();
}
