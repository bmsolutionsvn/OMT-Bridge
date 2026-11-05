#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "bench.h"
#include "net.h"
#include "relay.h"
#include "stream_id.h"

using namespace bmb;
using Clock = std::chrono::steady_clock;

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

struct Socket {
  SRTSOCKET s = SRT_INVALID_SOCK;
  ~Socket() {
    if (s != SRT_INVALID_SOCK) srt_close(s);
  }
  void close() {
    if (s != SRT_INVALID_SOCK) srt_close(s);
    s = SRT_INVALID_SOCK;
  }
};

std::string sid(const std::string& session, const std::string& bridge, const std::string& stream,
                StreamMode mode, const std::string& token = "t") {
  return formatStreamId(StreamId{session, bridge, stream, mode, token});
}

SRTSOCKET dial(int port, const std::string& streamId) {
  SRTSOCKET s = srt_create_socket();
  if (s == SRT_INVALID_SOCK) return s;
  applyOptions(s, SrtOptions{});
  if (srt_setsockflag(s, SRTO_STREAMID, streamId.data(), static_cast<int>(streamId.size())) == SRT_ERROR) {
    srt_close(s);
    return SRT_INVALID_SOCK;
  }
  int timeout = 200;
  srt_setsockflag(s, SRTO_RCVTIMEO, &timeout, sizeof timeout);
  auto addr = makeAddr("127.0.0.1", port);
  if (srt_connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == SRT_ERROR) {
    srt_close(s);
    return SRT_INVALID_SOCK;
  }
  return s;
}

int rejectReason(int port, const std::string& streamId) {
  SRTSOCKET s = srt_create_socket();
  applyOptions(s, SrtOptions{});
  srt_setsockflag(s, SRTO_STREAMID, streamId.data(), static_cast<int>(streamId.size()));
  auto addr = makeAddr("127.0.0.1", port);
  const int rc = srt_connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
  const int reason = rc == SRT_ERROR ? srt_getrejectreason(s) : 0;
  srt_close(s);
  return reason;
}

bool sendMsg(SRTSOCKET s, uint64_t seq, const std::string& payload = "") {
  std::vector<uint8_t> msg(kPayloadHeaderSize + std::max<size_t>(payload.size(), 1), 0);
  writeHeader(msg, PayloadHeader{seq, 0, 0, 0, 1, 0});
  std::memcpy(msg.data() + kPayloadHeaderSize, payload.data(),
              std::min(payload.size(), msg.size() - kPayloadHeaderSize));
  return srt_sendmsg2(s, reinterpret_cast<const char*>(msg.data()), static_cast<int>(msg.size()), nullptr) !=
         SRT_ERROR;
}

std::vector<uint64_t> readSeqs(SRTSOCKET s, size_t count, int timeoutMs = 8000) {
  std::vector<uint64_t> out;
  const auto end = Clock::now() + std::chrono::milliseconds(timeoutMs);
  std::vector<char> buf(4096);
  while (out.size() < count && Clock::now() < end) {
    const int n = srt_recvmsg2(s, buf.data(), static_cast<int>(buf.size()), nullptr);
    if (n > 0) {
      if (n >= static_cast<int>(kPayloadHeaderSize))
        out.push_back(readHeader(std::span<const uint8_t>(
                                     reinterpret_cast<const uint8_t*>(buf.data()), static_cast<size_t>(n)))
                          .seq);
    } else {
      const int err = srt_getlasterror(nullptr);
      if (err != SRT_ETIMEOUT && err != SRT_EASYNCRCV) break;
    }
  }
  return out;
}

std::string readPayload(SRTSOCKET s, int timeoutMs = 5000) {
  const auto end = Clock::now() + std::chrono::milliseconds(timeoutMs);
  std::vector<char> buf(4096);
  while (Clock::now() < end) {
    const int n = srt_recvmsg2(s, buf.data(), static_cast<int>(buf.size()), nullptr);
    if (n > static_cast<int>(kPayloadHeaderSize))
      return std::string(buf.data() + kPayloadHeaderSize, static_cast<size_t>(n) - kPayloadHeaderSize);
    if (n > 0) return std::string();
    const int err = srt_getlasterror(nullptr);
    if (err != SRT_ETIMEOUT && err != SRT_EASYNCRCV) break;
  }
  return {};
}

struct Summary {
  int64_t gaps = 0;
  std::string result;
};

std::string trim(std::string s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
  return s;
}

Summary lastSummary(const std::string& text) {
  std::istringstream in(text);
  std::string line, last;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty() && line.front() == '|' && line.find("bitrate") == std::string::npos) last = line;
  }
  REQUIRE_FALSE(last.empty());
  std::vector<std::string> cells;
  std::string cur;
  for (const char ch : last) {
    if (ch == '|') {
      cells.push_back(cur);
      cur.clear();
    } else {
      cur += ch;
    }
  }
  REQUIRE(cells.size() >= 11);
  Summary s;
  s.gaps = std::stoll(trim(cells[7]));
  s.result = trim(cells[10]);
  return s;
}

}  // namespace

TEST_CASE("Relay: a requester gets the publisher's messages in order") {
  RelayConfig cfg;
  cfg.bindHost = "127.0.0.1";
  cfg.port = freePort();
  cfg.token = "t";
  Relay relay(cfg);
  relay.start();

  Socket req{dial(cfg.port, sid("s", "h", "cam1", StreamMode::Request))};
  REQUIRE(req.s != SRT_INVALID_SOCK);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  Socket pub{dial(cfg.port, sid("s", "h", "cam1", StreamMode::Publish))};
  REQUIRE(pub.s != SRT_INVALID_SOCK);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  for (uint64_t i = 0; i < 1000; ++i) sendMsg(pub.s, i);
  const auto seqs = readSeqs(req.s, 1000, 10000);
  REQUIRE(seqs.size() == 1000);
  for (size_t i = 0; i < seqs.size(); ++i) CHECK(seqs[i] == i);
  relay.stop();
}

TEST_CASE("Relay: two requesters both receive") {
  RelayConfig cfg;
  cfg.bindHost = "127.0.0.1";
  cfg.port = freePort();
  cfg.token = "t";
  Relay relay(cfg);
  relay.start();

  Socket a{dial(cfg.port, sid("s", "h", "cam1", StreamMode::Request))};
  Socket b{dial(cfg.port, sid("s", "h", "cam1", StreamMode::Request))};
  REQUIRE(a.s != SRT_INVALID_SOCK);
  REQUIRE(b.s != SRT_INVALID_SOCK);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  Socket pub{dial(cfg.port, sid("s", "h", "cam1", StreamMode::Publish))};
  REQUIRE(pub.s != SRT_INVALID_SOCK);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  for (uint64_t i = 0; i < 100; ++i) sendMsg(pub.s, i);
  CHECK(readSeqs(a.s, 100).size() == 100);
  CHECK(readSeqs(b.s, 100).size() == 100);
  relay.stop();
}

TEST_CASE("Relay: a requester that comes first waits for the publisher") {
  RelayConfig cfg;
  cfg.bindHost = "127.0.0.1";
  cfg.port = freePort();
  cfg.token = "t";
  Relay relay(cfg);
  relay.start();

  Socket req{dial(cfg.port, sid("s", "h", "cam1", StreamMode::Request))};
  REQUIRE(req.s != SRT_INVALID_SOCK);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  Socket pub{dial(cfg.port, sid("s", "h", "cam1", StreamMode::Publish))};
  REQUIRE(pub.s != SRT_INVALID_SOCK);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  for (uint64_t i = 0; i < 50; ++i) sendMsg(pub.s, i);
  CHECK(readSeqs(req.s, 50).size() == 50);
  relay.stop();
}

TEST_CASE("Relay: a publisher can reconnect under a waiting requester") {
  RelayConfig cfg;
  cfg.bindHost = "127.0.0.1";
  cfg.port = freePort();
  cfg.token = "t";
  Relay relay(cfg);
  relay.start();

  Socket req{dial(cfg.port, sid("s", "h", "cam1", StreamMode::Request))};
  REQUIRE(req.s != SRT_INVALID_SOCK);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  {
    Socket pub{dial(cfg.port, sid("s", "h", "cam1", StreamMode::Publish))};
    REQUIRE(pub.s != SRT_INVALID_SOCK);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    for (uint64_t i = 0; i < 100; ++i) sendMsg(pub.s, i);
    CHECK(readSeqs(req.s, 100).size() == 100);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  Socket pub = {dial(cfg.port, sid("s", "h", "cam1", StreamMode::Publish))};
  REQUIRE(pub.s != SRT_INVALID_SOCK);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  for (uint64_t i = 0; i < 100; ++i) sendMsg(pub.s, i);
  CHECK(readSeqs(req.s, 100).size() == 100);
  relay.stop();
}

TEST_CASE("Relay: control hub forwards both ways") {
  RelayConfig cfg;
  cfg.bindHost = "127.0.0.1";
  cfg.port = freePort();
  cfg.token = "t";
  Relay relay(cfg);
  relay.start();

  Socket a{dial(cfg.port, sid("s", "h", "hub", StreamMode::Bidirectional))};
  Socket b{dial(cfg.port, sid("s", "h", "hub", StreamMode::Bidirectional))};
  REQUIRE(a.s != SRT_INVALID_SOCK);
  REQUIRE(b.s != SRT_INVALID_SOCK);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  CHECK(sendMsg(a.s, 1, "from-a"));
  CHECK(readPayload(b.s) == "from-a");
  CHECK(readSeqs(a.s, 1, 300).empty());  // no echo to the sender
  CHECK(sendMsg(b.s, 2, "from-b"));
  CHECK(readPayload(a.s) == "from-b");
  relay.stop();
}

TEST_CASE("Relay: bad stream id, wrong token and a second publisher are refused") {
  RelayConfig cfg;
  cfg.bindHost = "127.0.0.1";
  cfg.port = freePort();
  cfg.token = "t";
  Relay relay(cfg);
  relay.start();

  CHECK(rejectReason(cfg.port, "nonsense") == 1400);
  CHECK(rejectReason(cfg.port, sid("s", "h", "cam1", StreamMode::Publish, "wrong")) == 1401);

  Socket first{dial(cfg.port, sid("s", "h", "cam1", StreamMode::Publish))};
  REQUIRE(first.s != SRT_INVALID_SOCK);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(rejectReason(cfg.port, sid("s", "h", "cam1", StreamMode::Publish)) == 1409);
  relay.stop();
}

TEST_CASE("Relay: the total cap refuses a new publisher") {
  RelayConfig cfg;
  cfg.bindHost = "127.0.0.1";
  cfg.port = freePort();
  cfg.token = "t";
  cfg.maxTotalMbps = 5;
  Relay relay(cfg);
  relay.start();

  Socket pub{dial(cfg.port, sid("s", "h", "cam1", StreamMode::Publish))};
  REQUIRE(pub.s != SRT_INVALID_SOCK);
  std::vector<uint8_t> msg(1316, 0);
  std::thread flood([&] {
    for (int i = 0; i < 2000; ++i) {
      writeHeader(msg, PayloadHeader{static_cast<uint64_t>(i), 0, 0, 0, 1, 0});
      srt_sendmsg2(pub.s, reinterpret_cast<const char*>(msg.data()), static_cast<int>(msg.size()), nullptr);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  CHECK(rejectReason(cfg.port, sid("s", "h", "cam2", StreamMode::Publish)) == 1402);
  flood.join();
  relay.stop();
}

TEST_CASE("Relay: srt-bench passes through the relay") {
  RelayConfig cfg;
  cfg.bindHost = "127.0.0.1";
  cfg.port = freePort();
  cfg.token = "t";
  Relay relay(cfg);
  relay.start();

  BenchConfig recv;
  recv.role = "recv";
  recv.host = "127.0.0.1";
  recv.port = cfg.port;
  recv.streamId = sid("s", "relay", "cam1", StreamMode::Request);
  recv.bitrateBps = 10'000'000;
  recv.seconds = 3;

  BenchConfig send = recv;
  send.role = "send";
  send.streamId = sid("s", "relay", "cam1", StreamMode::Publish);

  std::ostringstream recvLog;
  int recvRc = -1;
  std::thread receiver([&] { recvRc = runBench(recv, recvLog); });
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  const int sendRc = runBench(send, recvLog);
  receiver.join();

  CHECK(sendRc == 0);
  CHECK(recvRc == 0);
  const auto s = lastSummary(recvLog.str());
  CHECK(s.gaps == 0);
  CHECK(s.result == "PASS");
  relay.stop();
}