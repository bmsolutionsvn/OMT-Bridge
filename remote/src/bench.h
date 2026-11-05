#pragma once

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>

#include "bench_payload.h"
#include "srt_util.h"

namespace bmb {

struct BenchConfig {
  std::string role = "send";  // send | recv
  std::string host;           // empty: connect to 127.0.0.1, listen on 0.0.0.0
  int port = 7000;
  std::string streamId;
  int64_t bitrateBps = 10'000'000;
  int seconds = 10;
  SrtOptions srt;
  std::optional<double> latencyRttFactor;
  bool listen = false;
  std::string csvPath;
};

// Exit code: 0 success, 2 the RTT probe got no reply, 1 any other failure.
int runBench(const BenchConfig&, std::ostream& log);

}  // namespace bmb