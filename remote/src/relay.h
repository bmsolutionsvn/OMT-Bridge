#pragma once

#include <memory>
#include <string>
#include <vector>

namespace bmb {

struct RelayConfig {
  std::string bindHost = "0.0.0.0";
  int port = 7000;
  std::string passphrase;
  std::string token;
  double maxTotalMbps = 0;  // 0 = unlimited
  int latencyMs = 120;
};

class Relay {
 public:
  explicit Relay(RelayConfig);
  ~Relay();
  Relay(const Relay&) = delete;
  Relay& operator=(const Relay&) = delete;

  void start();
  void stop();

  struct SessionStats {
    std::string session;
    double mbpsIn, mbpsOut;
    int publishers, requesters;
  };
  std::vector<SessionStats> stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bmb