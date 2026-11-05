#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "bench.h"
#include "net.h"

using namespace bmb;

namespace {

int freePort() {
  SrtLibrary lib;
  SRTSOCKET s = srt_create_socket();
  auto addr = makeAddr("127.0.0.1", 0);
  REQUIRE(srt_bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != SRT_ERROR);
  int len = sizeof addr;
  REQUIRE(srt_getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) != SRT_ERROR);
  const int port = portOf(addr);
  srt_close(s);
  return port;
}

std::string trim(std::string s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
  return s;
}

struct Summary {
  double mbps = 0;
  int64_t dropRecv = 0;
  int64_t gaps = 0;
  int64_t sndDrop = 0;
  std::string result;
};

Summary lastSummary(const std::string& text) {
  std::istringstream in(text);
  std::string line, last;
  while (std::getline(in, line)) {
    line = trim(line);
    if (!line.empty() && line.front() == '|' && line.find("bitrate") == std::string::npos) last = line;
  }
  REQUIRE_FALSE(last.empty());
  std::vector<std::string> cells;
  std::string cur;
  for (const char ch : last) {
    if (ch == '|') {
      cells.push_back(trim(cur));
      cur.clear();
    } else {
      cur += ch;
    }
  }
  cells.push_back(trim(cur));
  REQUIRE(cells.size() >= 11);
  Summary s;
  s.mbps = std::stod(cells[4]);
  s.dropRecv = std::stoll(cells[6]);
  s.gaps = std::stoll(cells[7]);
  s.sndDrop = std::stoll(cells[8]);
  s.result = cells[10];
  return s;
}

BenchConfig benchConfig(const std::string& role, int port, int mbps, int seconds) {
  BenchConfig c;
  c.role = role;
  c.host = "127.0.0.1";
  c.port = port;
  c.bitrateBps = static_cast<int64_t>(mbps) * 1'000'000;
  c.seconds = seconds;
  c.listen = role == "recv";
  return c;
}

}  // namespace

TEST_CASE("Bench: frame sizes keep the average") {
  const auto sizes = frameSizes(40'000'000, 60, 120, 4.0);
  REQUIRE(sizes.size() == 120);
  CHECK(std::abs(sizes[0] - 4 * sizes[1]) <= 1);
  const int64_t sum = std::accumulate(sizes.begin(), sizes.end(), int64_t{0});
  CHECK(std::abs(sum - (40'000'000 / 8 * 2)) <= 120);
}

TEST_CASE("Bench: header round trip") {
  std::vector<uint8_t> buf(kPayloadHeaderSize, 0);
  const PayloadHeader in{1234567890123ull, 987654321, 42, 3, 7, 0xdeadbeefcafeull};
  writeHeader(buf, in);
  const auto out = readHeader(buf);
  CHECK(out.seq == in.seq);
  CHECK(out.sendUs == in.sendUs);
  CHECK(out.frame == in.frame);
  CHECK(out.part == in.part);
  CHECK(out.parts == in.parts);
  CHECK(out.reserved == in.reserved);
}

TEST_CASE("Bench: seq tracker counts only missing numbers") {
  SeqTracker t;
  for (const uint64_t s : {0ull, 1ull, 2ull, 4ull, 3ull, 4ull, 7ull}) t.add(s);
  CHECK(t.received() == 6);
  CHECK(t.gaps() == 2);
}

TEST_CASE("Bench: 10 Mbps for 3 s over loopback passes") {
  const int port = freePort();
  const auto recvCfg = benchConfig("recv", port, 10, 3);
  const auto sendCfg = benchConfig("send", port, 10, 3);
  std::ostringstream recvLog;
  std::ostringstream sendLog;
  int recvRc = -1;
  std::thread receiver([&] { recvRc = runBench(recvCfg, recvLog); });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const int sendRc = runBench(sendCfg, sendLog);
  receiver.join();

  CHECK(sendRc == 0);
  CHECK(recvRc == 0);
  const auto s = lastSummary(recvLog.str());
  CHECK(s.gaps == 0);
  CHECK(s.mbps > 9.0);
  CHECK(s.mbps < 11.0);
  CHECK(s.result == "PASS");
}

TEST_CASE("Bench: a failed RTT probe stops with a message") {
  const int port = freePort();
  auto cfg = benchConfig("send", port, 10, 1);
  cfg.latencyRttFactor = 4.0;
  std::ostringstream log;
  CHECK(runBench(cfg, log) == 2);
  CHECK(log.str().find("RTT probe failed") != std::string::npos);
}

TEST_CASE("Bench: a too-small send buffer shows as snd_drop") {
  const int port = freePort();
  const auto recvCfg = benchConfig("recv", port, 50, 3);
  auto sendCfg = benchConfig("send", port, 50, 2);
  sendCfg.srt.sndBufBytes = 64 * 1024;
  std::ostringstream recvLog;
  std::ostringstream sendLog;
  int recvRc = -1;
  std::thread receiver([&] { recvRc = runBench(recvCfg, recvLog); });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const auto start = std::chrono::steady_clock::now();
  const int sendRc = runBench(sendCfg, sendLog);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  receiver.join();

  CHECK(sendRc == 0);
  CHECK(recvRc == 0);
  const auto s = lastSummary(sendLog.str());
  CHECK(s.sndDrop > 0);
  CHECK(s.result != "PASS");
  CHECK(elapsed <= std::chrono::seconds(3));
}