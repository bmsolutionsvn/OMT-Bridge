#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "relay.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void onSignal(int) { g_stop = 1; }

std::string envOr(const char* name, const std::string& fallback) {
  const char* value = std::getenv(name);
  return value ? std::string(value) : fallback;
}

double number(const std::string& text, const std::string& what) {
  size_t used = 0;
  double value = 0;
  try {
    value = std::stod(text, &used);
  } catch (const std::exception&) {
    throw std::runtime_error(what + " needs a number, got '" + text + "'");
  }
  if (used != text.size()) throw std::runtime_error(what + " needs a number, got '" + text + "'");
  return value;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  for (const auto& a : args) {
    if (a == "--help" || a == "-h") {
      std::cout << "bmb-relay [--port N] [--bind H] [--passphrase P] [--token T]\n"
                   "          [--max-mbps X] [--latency-ms N]\n"
                   "Environment: BMB_RELAY_PORT, BMB_RELAY_PASSPHRASE, BMB_RELAY_TOKEN,\n"
                   "             BMB_RELAY_MAX_MBPS, BMB_RELAY_LATENCY_MS\n";
      return 0;
    }
  }

  bmb::RelayConfig cfg;
  try {
    if (!envOr("BMB_RELAY_PORT", "").empty()) cfg.port = static_cast<int>(number(envOr("BMB_RELAY_PORT", ""), "BMB_RELAY_PORT"));
    cfg.passphrase = envOr("BMB_RELAY_PASSPHRASE", cfg.passphrase);
    cfg.token = envOr("BMB_RELAY_TOKEN", cfg.token);
    if (!envOr("BMB_RELAY_MAX_MBPS", "").empty())
      cfg.maxTotalMbps = number(envOr("BMB_RELAY_MAX_MBPS", ""), "BMB_RELAY_MAX_MBPS");
    if (!envOr("BMB_RELAY_LATENCY_MS", "").empty())
      cfg.latencyMs = static_cast<int>(number(envOr("BMB_RELAY_LATENCY_MS", ""), "BMB_RELAY_LATENCY_MS"));

    for (size_t i = 0; i < args.size(); ++i) {
      const std::string& flag = args[i];
      auto value = [&]() -> std::string {
        if (i + 1 >= args.size()) throw std::runtime_error(flag + " needs a value");
        return args[++i];
      };
      if (flag == "--port") cfg.port = static_cast<int>(number(value(), flag));
      else if (flag == "--bind") cfg.bindHost = value();
      else if (flag == "--passphrase") cfg.passphrase = value();
      else if (flag == "--token") cfg.token = value();
      else if (flag == "--max-mbps") cfg.maxTotalMbps = number(value(), flag);
      else if (flag == "--latency-ms") cfg.latencyMs = static_cast<int>(number(value(), flag));
      else throw std::runtime_error("unknown argument '" + flag + "'");
    }
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return 64;
  }

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  try {
    bmb::Relay relay(cfg);
    relay.start();
    std::cout << "bmb-relay listening on " << cfg.bindHost << ":" << cfg.port << "\n";
    std::cout.flush();
    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    relay.stop();
  } catch (const std::exception& e) {
    std::cerr << "bmb-relay: " << e.what() << "\n";
    return 1;
  }
  return 0;
}