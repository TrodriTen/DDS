// SPDX-License-Identifier: Apache-2.0
// Data Writer (publicador) del benchmark sobre FastDdsBackend.
#include <iostream>

#include "fastdds_backend.hpp"
#include "bench/publisher.hpp"

int main(int argc, char** argv) {
  bench::BenchConfig cfg;
  std::string error;
  if (!bench::parse_cli(argc, argv, "pub", "fastdds", cfg, error)) {
    std::cerr << "error: " << error << " (usa --help)" << std::endl;
    return 1;
  }
  bench::resolve_default_configs(cfg);
  return bench::PublisherApp<bench::FastDdsBackend>(std::move(cfg)).run();
}
