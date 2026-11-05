#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "bench.h"

namespace {

const char* kUsage =
    "srt-bench send|recv --host H --port P [--listen] [--streamid S] [--mbps N] [--seconds N]\n"
    "          [--latency MS | --latency-rtt X] [--sndbuf B] [--rcvbuf B] [--fc N] [--payload N]\n"
    "          [--overhead PCT] [--passphrase P] [--csv FILE]\n"
    "\n"
    "  send            paced publisher: 60 bursts/s, a keyframe burst every 2 s\n"
    "  recv            requester (or --listen for a publisher to dial in)\n"
    "  --host H        IPv4 address of the peer or relay (default 127.0.0.1)\n"
    "  --latency-rtt X latency = max(120, X * measured RTT); a failed probe exits 2\n"
    "  --sndbuf/--rcvbuf B  bytes, or K/M/G (KiB/MiB/GiB) (default: libsrt)\n"
    "  --streamid S    full SRT stream id, e.g. '#!::r=bmb/s/b/cam1,m=publish'\n";

bool endsWith(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

int64_t parseNumber(const std::string& text, const std::string& flag) {
  size_t used = 0;
  long long value = 0;
  try {
    value = std::stoll(text, &used);
  } catch (const std::exception&) {
    throw std::runtime_error(flag + " needs a number, got '" + text + "'");
  }
  if (used != text.size()) throw std::runtime_error(flag + " needs a number, got '" + text + "'");
  return value;
}

double parseDouble(const std::string& text, const std::string& flag) {
  size_t used = 0;
  double value = 0;
  try {
    value = std::stod(text, &used);
  } catch (const std::exception&) {
    throw std::runtime_error(flag + " needs a number, got '" + text + "'");
  }
  if (used != text.size()) throw std::runtime_error(flag + " needs a number, got '" + text + "'");
  return value;
}

int64_t parseBytes(const std::string& text, const std::string& flag) {
  std::string digits = text;
  int64_t scale = 1;
  std::string suffix;
  while (!digits.empty() && std::isalpha(static_cast<unsigned char>(digits.back()))) {
    suffix.insert(suffix.begin(), static_cast<char>(std::tolower(static_cast<unsigned char>(digits.back()))));
    digits.pop_back();
  }
  if (suffix == "k" || suffix == "kb" || suffix == "kib") scale = 1024;
  else if (suffix == "m" || suffix == "mb" || suffix == "mib") scale = 1024 * 1024;
  else if (suffix == "g" || suffix == "gb" || suffix == "gib") scale = 1024 * 1024 * 1024;
  else if (!suffix.empty()) throw std::runtime_error(flag + " has an unknown size suffix '" + text + "'");
  const int64_t n = parseNumber(digits, flag);
  if (n < 0) throw std::runtime_error(flag + " cannot be negative");
  return n * scale;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  for (const auto& a : args) {
    if (a == "--help" || a == "-h") {
      std::cout << kUsage;
      return 0;
    }
  }
  if (args.empty() || (args[0] != "send" && args[0] != "recv")) {
    std::cerr << "expected 'send' or 'recv'\n" << kUsage;
    return 64;
  }

  bmb::BenchConfig cfg;
  cfg.role = args[0];
  try {
    for (size_t i = 1; i < args.size(); ++i) {
      const std::string& flag = args[i];
      auto value = [&]() -> std::string {
        if (i + 1 >= args.size()) throw std::runtime_error(flag + " needs a value");
        return args[++i];
      };
      if (flag == "--host") cfg.host = value();
      else if (flag == "--port") cfg.port = static_cast<int>(parseNumber(value(), flag));
      else if (flag == "--listen") cfg.listen = true;
      else if (flag == "--streamid") cfg.streamId = value();
      else if (flag == "--mbps") cfg.bitrateBps = static_cast<int64_t>(parseDouble(value(), flag) * 1e6);
      else if (flag == "--seconds") cfg.seconds = static_cast<int>(parseNumber(value(), flag));
      else if (flag == "--latency") cfg.srt.latencyMs = static_cast<int>(parseNumber(value(), flag));
      else if (flag == "--latency-rtt") cfg.latencyRttFactor = parseDouble(value(), flag);
      else if (flag == "--sndbuf") cfg.srt.sndBufBytes = static_cast<int>(parseBytes(value(), flag));
      else if (flag == "--rcvbuf") cfg.srt.rcvBufBytes = static_cast<int>(parseBytes(value(), flag));
      else if (flag == "--fc") cfg.srt.fcPackets = static_cast<int>(parseNumber(value(), flag));
      else if (flag == "--payload") cfg.srt.payloadSize = static_cast<int>(parseNumber(value(), flag));
      else if (flag == "--overhead") cfg.srt.overheadPct = static_cast<int>(parseNumber(value(), flag));
      else if (flag == "--passphrase") cfg.srt.passphrase = value();
      else if (flag == "--csv") cfg.csvPath = value();
      else throw std::runtime_error("unknown argument '" + flag + "'");
    }
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n" << kUsage;
    return 64;
  }

  const int rc = bmb::runBench(cfg, std::cout);
  return rc;
}