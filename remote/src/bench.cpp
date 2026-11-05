#include "bench.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "net.h"

namespace bmb {
namespace {

using Clock = std::chrono::steady_clock;

struct Socket {
  SRTSOCKET s = SRT_INVALID_SOCK;
  ~Socket() {
    if (s != SRT_INVALID_SOCK) srt_close(s);
  }
};

void setFlag(SRTSOCKET s, SRT_SOCKOPT opt, int value) {
  if (srt_setsockflag(s, opt, &value, sizeof value) == SRT_ERROR)
    throw std::runtime_error(std::string("SRT option: ") + srt_getlasterror_str());
}

void setStreamId(SRTSOCKET s, const std::string& id) {
  if (id.empty()) return;
  if (srt_setsockflag(s, SRTO_STREAMID, id.data(), static_cast<int>(id.size())) == SRT_ERROR)
    throw std::runtime_error(std::string("SRT stream id: ") + srt_getlasterror_str());
}

void connectTo(SRTSOCKET s, const BenchConfig& cfg) {
  auto addr = makeAddr(cfg.host.empty() ? "127.0.0.1" : cfg.host, cfg.port);
  if (srt_connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == SRT_ERROR)
    throw std::runtime_error("connect " + (cfg.host.empty() ? "127.0.0.1" : cfg.host) + ":" +
                             std::to_string(cfg.port) + ": " + srt_getlasterror_str());
}

void csvRow(std::ostream& out, double t, const SrtStats& st, int64_t gaps, int64_t sndDrop) {
  out << std::fixed << std::setprecision(2) << t << ',' << st.mbpsSent << ',' << st.mbpsRecv << ','
      << st.pktSent << ',' << st.pktRetrans << ',' << st.pktLossRecv << ',' << st.pktDropRecv << ','
      << st.pktDropSend << ',' << st.rttMs << ',' << st.sndBufMs << ',' << st.rcvBufMs << ','
      << st.estBwMbps << ',' << gaps << ',' << sndDrop << '\n';
}

void writeSummary(std::ostream& log, const BenchConfig& cfg, int latencyMs, double mbps,
                  const SrtStats& st, int64_t gaps, int64_t sndDrop, double rttAvg) {
  const double retrans = st.pktSent > 0 ? 100.0 * static_cast<double>(st.pktRetrans) / st.pktSent : 0.0;
  const bool pass = st.pktDropRecv == 0 && gaps == 0 && sndDrop == 0;
  std::ostringstream line;
  line << std::fixed << std::setprecision(2);
  line << "| bitrate | latency | options | mbps avg | retrans % | drop_recv | seq_gaps | snd_drop | rtt avg | result |\n";
  line << "| " << static_cast<double>(cfg.bitrateBps) / 1e6 << " | " << latencyMs << " | payload="
       << cfg.srt.payloadSize << " sndbuf=" << cfg.srt.sndBufBytes << " rcvbuf=" << cfg.srt.rcvBufBytes
       << " fc=" << cfg.srt.fcPackets << " oh=" << cfg.srt.overheadPct << "%"
       << (cfg.srt.passphrase.empty() ? "" : " aes") << " | " << mbps << " | " << retrans << " | "
       << st.pktDropRecv << " | " << gaps << " | " << sndDrop << " | " << rttAvg << " | "
       << (pass ? "PASS" : "FAIL") << " |\n";
  log << line.str();
}

class Csv {
 public:
  explicit Csv(const std::string& path) {
    if (path.empty()) return;
    file_.open(path, std::ios::out | std::ios::trunc);
    if (!file_) return;
    file_ << "t_s,mbps_sent,mbps_recv,pkt_sent,pkt_retrans,pkt_loss_recv,pkt_drop_recv,"
             "pkt_drop_send,rtt_ms,snd_buf_ms,rcv_buf_ms,est_bw_mbps,seq_gaps,snd_drop\n";
    out_ = &file_;
  }
  void row(double t, const SrtStats& st, int64_t gaps, int64_t sndDrop) {
    if (out_) csvRow(*out_, t, st, gaps, sndDrop);
  }

 private:
  std::ofstream file_;
  std::ostream* out_ = nullptr;
};

bool waitForRtt(SRTSOCKET s, double& rtt) {
  const auto end = Clock::now() + std::chrono::seconds(3);
  while (Clock::now() < end) {
    try {
      const auto st = statsOf(s, false);
      if (st.rttMs > 0) {
        rtt = st.rttMs;
        return true;
      }
    } catch (const std::exception&) {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

double secondsSince(Clock::time_point t) {
  return std::chrono::duration<double>(Clock::now() - t).count();
}

int runSend(const BenchConfig& cfg, std::ostream& log) {
  int latencyMs = cfg.srt.latencyMs;
  if (cfg.latencyRttFactor) {
    Socket probe;
    probe.s = srt_create_socket();
    if (probe.s == SRT_INVALID_SOCK)
      throw std::runtime_error(std::string("srt_create_socket: ") + srt_getlasterror_str());
    applyOptions(probe.s, cfg.srt);
    setStreamId(probe.s, cfg.streamId);
    double rtt = 0;
    bool ok = false;
    try {
      connectTo(probe.s, cfg);
      ok = waitForRtt(probe.s, rtt);
    } catch (const std::exception&) {
      ok = false;
    }
    if (!ok) {
      log << "RTT probe failed: no reply from " << (cfg.host.empty() ? "127.0.0.1" : cfg.host) << ":"
          << cfg.port << "\n";
      return 2;
    }
    latencyMs = std::max(120, static_cast<int>(std::lround(cfg.latencyRttFactor.value() * rtt)));
  }

  Socket sock;
  sock.s = srt_create_socket();
  if (sock.s == SRT_INVALID_SOCK)
    throw std::runtime_error(std::string("srt_create_socket: ") + srt_getlasterror_str());
  SrtOptions opts = cfg.srt;
  opts.latencyMs = latencyMs;
  applyOptions(sock.s, opts);
  setStreamId(sock.s, cfg.streamId);
  setFlag(sock.s, SRTO_SNDSYN, 0);  // a full buffer is reported as snd_drop, never blocks pacing
  connectTo(sock.s, cfg);

  const auto sizes = frameSizes(cfg.bitrateBps, 60, 120, 4.0);
  const int payload = std::max(static_cast<int>(kPayloadHeaderSize), cfg.srt.payloadSize);
  std::vector<uint8_t> msg(static_cast<size_t>(payload));
  Csv csv(cfg.csvPath);

  uint64_t seq = 0;
  int64_t bytes = 0;
  int64_t sndDrop = 0;
  double rttSum = 0;
  int rttCount = 0;
  const auto start = Clock::now();
  auto nextReport = start + std::chrono::seconds(1);
  const int frames = std::max(1, cfg.seconds) * 60;

  for (int f = 0; f < frames; ++f) {
    const auto due = start + std::chrono::microseconds(static_cast<int64_t>(f) * 1'000'000 / 60);
    std::this_thread::sleep_until(due);
    const int size = sizes[static_cast<size_t>(f) % sizes.size()];
    const int parts = std::max(1, (size + payload - 1) / payload);
    for (int part = 0; part < parts; ++part) {
      const int n = std::min(payload, size - part * payload);
      if (n <= 0) break;
      const auto nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                             Clock::now().time_since_epoch())
                             .count();
      const PayloadHeader h{seq, nowUs, static_cast<uint32_t>(f), static_cast<uint16_t>(part),
                            static_cast<uint16_t>(parts), 0};
      writeHeader(std::span<uint8_t>(msg.data(), static_cast<size_t>(n)), h);
      if (srt_sendmsg2(sock.s, reinterpret_cast<const char*>(msg.data()), n, nullptr) == SRT_ERROR)
        ++sndDrop;
      else
        bytes += n;
      ++seq;
    }
    if (Clock::now() >= nextReport) {
      try {
        const auto st = statsOf(sock.s, true);
        if (st.rttMs > 0) {
          rttSum += st.rttMs;
          ++rttCount;
        }
        csv.row(secondsSince(start), st, 0, sndDrop);
      } catch (const std::exception&) {
      }
      nextReport += std::chrono::seconds(1);
    }
  }

  const double elapsed = std::max(0.001, secondsSince(start));
  SrtStats st{};
  try {
    st = statsOf(sock.s, false);
  } catch (const std::exception&) {
    // The receiver may close at the same moment the run ends; the send is still complete.
  }
  const double mbps = static_cast<double>(bytes) * 8.0 / elapsed / 1e6;
  const double rttAvg = rttCount > 0 ? rttSum / rttCount : st.rttMs;
  writeSummary(log, cfg, latencyMs, mbps, st, 0, sndDrop, rttAvg);
  return 0;
}

struct RecvTotals {
  int64_t bytes = 0;
  int64_t dropRecv = 0;
  double rttSum = 0;
  int rttCount = 0;
  bool gotMessage = false;
  Clock::time_point firstMessage{};
  SeqTracker tracker;
};

// Drains one connection until it closes or `cap` passes. Returns true when at least one message
// arrived; the caller uses that to tell a real sender from an RTT probe or a failed dial.
bool drain(SRTSOCKET c, const BenchConfig& cfg, Clock::time_point cap, Csv& csv, RecvTotals& t) {
  std::vector<char> buf(static_cast<size_t>(std::max(2048, cfg.srt.payloadSize)));
  auto nextReport = Clock::now() + std::chrono::seconds(1);
  while (Clock::now() < cap) {
    const int n = srt_recvmsg2(c, buf.data(), static_cast<int>(buf.size()), nullptr);
    if (n > 0) {
      if (!t.gotMessage) {
        t.gotMessage = true;
        t.firstMessage = Clock::now();
      }
      t.bytes += n;
      if (n >= static_cast<int>(kPayloadHeaderSize)) {
        const auto h = readHeader(std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(buf.data()), static_cast<size_t>(n)));
        t.tracker.add(h.seq);
      }
    } else {
      const int err = srt_getlasterror(nullptr);
      if (err == SRT_EASYNCRCV || err == SRT_ETIMEOUT) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      } else {
        break;  // peer closed or the connection was lost
      }
    }
    if (t.gotMessage && Clock::now() >= nextReport) {
      try {
        const auto st = statsOf(c, true);
        if (st.rttMs > 0) {
          t.rttSum += st.rttMs;
          ++t.rttCount;
        }
        csv.row(secondsSince(t.firstMessage), st, t.tracker.gaps(), 0);
      } catch (const std::exception&) {
      }
      nextReport += std::chrono::seconds(1);
    }
  }
  try {
    t.dropRecv += statsOf(c, false).pktDropRecv;
  } catch (const std::exception&) {
  }
  return t.gotMessage;
}

int runRecv(const BenchConfig& cfg, std::ostream& log) {
  Socket sock;
  sock.s = srt_create_socket();
  if (sock.s == SRT_INVALID_SOCK)
    throw std::runtime_error(std::string("srt_create_socket: ") + srt_getlasterror_str());
  applyOptions(sock.s, cfg.srt);
  Csv csv(cfg.csvPath);

  const auto start = Clock::now();
  const auto cap = start + std::chrono::seconds(std::max(1, cfg.seconds) + 1);
  RecvTotals totals;
  int64_t gaps = 0;

  if (cfg.listen) {
    // A blocking accept with a timeout, not non-blocking: libsrt logs every EASYNCRCV accept
    // as an error, which floods the bench output.
    setFlag(sock.s, SRTO_RCVTIMEO, 200);
    auto addr = makeAddr(cfg.host.empty() ? "0.0.0.0" : cfg.host, cfg.port);
    if (srt_bind(sock.s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == SRT_ERROR)
      throw std::runtime_error(std::string("bind: ") + srt_getlasterror_str());
    if (srt_listen(sock.s, 10) == SRT_ERROR)
      throw std::runtime_error(std::string("listen: ") + srt_getlasterror_str());

    while (Clock::now() < cap) {
      const SRTSOCKET c = srt_accept(sock.s, nullptr, nullptr);
      if (c == SRT_INVALID_SOCK) {
        const int err = srt_getlasterror(nullptr);
        if (err != SRT_EASYNCRCV && err != SRT_ETIMEOUT)
          throw std::runtime_error(std::string("accept: ") + srt_getlasterror_str());
        continue;
      }
      setFlag(c, SRTO_RCVSYN, 0);
      const bool got = drain(c, cfg, cap, csv, totals);
      srt_close(c);
      if (got) break;  // a probe or a failed dial sends nothing; keep waiting for the real sender
    }
    gaps = totals.tracker.gaps();
  } else {
    setStreamId(sock.s, cfg.streamId);
    connectTo(sock.s, cfg);
    setFlag(sock.s, SRTO_RCVSYN, 0);
    drain(sock.s, cfg, cap, csv, totals);
    gaps = totals.tracker.gaps();
  }

  const double elapsed =
      totals.gotMessage ? std::max(0.001, secondsSince(totals.firstMessage)) : 0.001;
  const double mbps = static_cast<double>(totals.bytes) * 8.0 / elapsed / 1e6;
  const double rttAvg = totals.rttCount > 0 ? totals.rttSum / totals.rttCount : 0.0;
  SrtStats st{};
  st.pktDropRecv = totals.dropRecv;
  writeSummary(log, cfg, cfg.srt.latencyMs, mbps, st, gaps, 0, rttAvg);
  return 0;
}

}  // namespace

int runBench(const BenchConfig& cfg, std::ostream& log) {
  static SrtLibrary library;
  try {
    return cfg.role == "recv" ? runRecv(cfg, log) : runSend(cfg, log);
  } catch (const std::exception& e) {
    log << "error: " << e.what() << "\n";
    return 1;
  }
}

}  // namespace bmb