#include "relay.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include <srt/access_control.h>

#include "net.h"
#include "srt_util.h"
#include "stream_id.h"

namespace bmb {
namespace {

using Clock = std::chrono::steady_clock;

void setFlag(SRTSOCKET s, SRT_SOCKOPT opt, int value) {
  if (srt_setsockflag(s, opt, &value, sizeof value) == SRT_ERROR)
    throw std::runtime_error(std::string("SRT option: ") + srt_getlasterror_str());
}

}  // namespace

struct Relay::Impl {
  explicit Impl(RelayConfig c) : cfg(std::move(c)) {}

  struct Session {
    SRTSOCKET publisher = SRT_INVALID_SOCK;
    std::vector<SRTSOCKET> requesters;
    std::vector<SRTSOCKET> hub;
    int64_t bytesIn = 0, bytesOut = 0;
    Clock::time_point started = Clock::now();
  };

  RelayConfig cfg;
  SrtLibrary srt;
  SRTSOCKET listener = SRT_INVALID_SOCK;
  int epfd = -1;
  std::thread thread;
  std::atomic<bool> stopping{false};

  mutable std::mutex mu;
  std::map<std::string, Session> sessions;
  std::map<SRTSOCKET, std::string> resourceOfSocket;
  std::map<SRTSOCKET, StreamMode> modeOfSocket;
  std::map<std::string, Clock::time_point> pendingPublish;
  int64_t bytesInWindow = 0;
  Clock::time_point windowStart = Clock::now();
  double totalMbps = 0;

  static int onHandshake(void* opaque, SRTSOCKET ns, int, const sockaddr*, const char* streamid) {
    auto* self = static_cast<Impl*>(opaque);
    const auto id = streamid ? parseStreamId(streamid) : std::nullopt;
    if (!id) {
      srt_setrejectreason(ns, SRT_REJX_BAD_REQUEST);
      return -1;
    }
    int reject = 0;
    {
      // Never call into libsrt while holding mu: the handshake itself runs under SRT's own lock.
      std::lock_guard lock(self->mu);
      if (!self->cfg.token.empty() && id->token != self->cfg.token) {
        reject = SRT_REJX_UNAUTHORIZED;
      } else if (id->mode == StreamMode::Publish) {
        const auto now = Clock::now();
        for (auto it = self->pendingPublish.begin(); it != self->pendingPublish.end();) {
          if (now - it->second > std::chrono::seconds(5)) it = self->pendingPublish.erase(it);
          else ++it;
        }
        const std::string res = resourceOf(*id);
        const auto sit = self->sessions.find(res);
        const bool taken = (sit != self->sessions.end() && sit->second.publisher != SRT_INVALID_SOCK) ||
                           self->pendingPublish.count(res) > 0;
        if (taken) {
          reject = SRT_REJX_CONFLICT;
        } else if (self->cfg.maxTotalMbps > 0 && self->totalMbps >= self->cfg.maxTotalMbps) {
          reject = SRT_REJX_OVERLOAD;
        } else {
          self->pendingPublish[res] = now;
        }
      }
    }
    if (reject != 0) {
      srt_setrejectreason(ns, reject);
      return -1;
    }
    return 0;
  }

  void start() {
    listener = srt_create_socket();
    if (listener == SRT_INVALID_SOCK)
      throw std::runtime_error(std::string("srt_create_socket: ") + srt_getlasterror_str());
    SrtOptions opts;
    opts.latencyMs = cfg.latencyMs;
    opts.passphrase = cfg.passphrase;
    applyOptions(listener, opts);
    setFlag(listener, SRTO_RCVSYN, 0);
    if (srt_listen_callback(listener, &Impl::onHandshake, this) == SRT_ERROR)
      throw std::runtime_error(std::string("srt_listen_callback: ") + srt_getlasterror_str());
    auto addr = makeAddr(cfg.bindHost, cfg.port);
    if (srt_bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == SRT_ERROR)
      throw std::runtime_error(std::string("bind ") + cfg.bindHost + ":" + std::to_string(cfg.port) +
                               ": " + srt_getlasterror_str());
    if (srt_listen(listener, 128) == SRT_ERROR)
      throw std::runtime_error(std::string("listen: ") + srt_getlasterror_str());

    epfd = srt_epoll_create();
    if (epfd < 0) throw std::runtime_error(std::string("srt_epoll_create: ") + srt_getlasterror_str());
    const int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
    if (srt_epoll_add_usock(epfd, listener, &events) == SRT_ERROR)
      throw std::runtime_error(std::string("srt_epoll_add_usock: ") + srt_getlasterror_str());

    thread = std::thread([this] { loop(); });
  }

  void stop() {
    if (!thread.joinable()) return;
    stopping = true;
    thread.join();
    closeAll();
  }

  void closeAll() {
    std::vector<SRTSOCKET> toClose;
    SRTSOCKET lst = SRT_INVALID_SOCK;
    int ep = -1;
    {
      std::lock_guard lock(mu);
      for (auto& [sock, res] : resourceOfSocket) toClose.push_back(sock);
      resourceOfSocket.clear();
      modeOfSocket.clear();
      sessions.clear();
      pendingPublish.clear();
      lst = listener;
      listener = SRT_INVALID_SOCK;
      ep = epfd;
      epfd = -1;
    }
    // No libsrt call while mu is held (see onHandshake); srt_close unregisters from the epoll.
    for (const SRTSOCKET s : toClose) srt_close(s);
    if (lst != SRT_INVALID_SOCK) srt_close(lst);
    if (ep >= 0) srt_epoll_release(ep);
  }

  void loop() {
    std::vector<char> buf(65536);
    SRTSOCKET readfds[256];
    auto nextTick = Clock::now() + std::chrono::seconds(1);
    auto nextLog = Clock::now() + std::chrono::seconds(10);
    while (!stopping) {
      int count = 256;
      const int ready = srt_epoll_wait(epfd, readfds, &count, nullptr, nullptr, 200, nullptr, nullptr,
                                       nullptr, nullptr);
      if (ready > 0) {
        for (int i = 0; i < count; ++i) {
          if (readfds[i] == listener) acceptOne();
          else handleRead(readfds[i], buf);
        }
      }
      const auto now = Clock::now();
      if (now >= nextTick) {
        measureInput();
        nextTick += std::chrono::seconds(1);
      }
      if (now >= nextLog) {
        printStats();
        nextLog += std::chrono::seconds(10);
      }
    }
  }

  void acceptOne() {
    // One accept per readiness: the epoll is level-triggered and reports again if more wait.
    // A loop would end on an EASYNCRCV accept, which libsrt logs as an error.
    SRTSOCKET c = srt_accept(listener, nullptr, nullptr);
    if (c != SRT_INVALID_SOCK) addSocket(c);
  }

  void addSocket(SRTSOCKET c) {
    char idbuf[513]{};
    int len = 512;
    if (srt_getsockflag(c, SRTO_STREAMID, idbuf, &len) == SRT_ERROR) {
      srt_close(c);
      return;
    }
    const auto id = parseStreamId(std::string_view(idbuf, static_cast<size_t>(std::max(0, len))));
    if (!id) {
      srt_close(c);
      return;
    }
    setFlag(c, SRTO_RCVSYN, 0);
    setFlag(c, SRTO_SNDSYN, 0);  // a full requester buffer drops that message, never stalls the relay
    const std::string res = resourceOf(*id);
    {
      std::lock_guard lock(mu);
      resourceOfSocket[c] = res;
      modeOfSocket[c] = id->mode;
      auto& session = sessions[res];
      if (id->mode == StreamMode::Publish) {
        session.publisher = c;
        pendingPublish.erase(res);
      } else if (id->mode == StreamMode::Request) {
        session.requesters.push_back(c);
      } else {
        session.hub.push_back(c);
      }
    }
    const int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
    srt_epoll_add_usock(epfd, c, &events);
  }

  void handleRead(SRTSOCKET s, std::vector<char>& buf) {
    const int n = srt_recvmsg2(s, buf.data(), static_cast<int>(buf.size()), nullptr);
    if (n <= 0) {
      const int err = srt_getlasterror(nullptr);
      if (err == SRT_EASYNCRCV || err == SRT_ETIMEOUT) return;
      removeSocket(s);
      return;
    }

    std::vector<SRTSOCKET> targets;
    {
      std::lock_guard lock(mu);
      const auto rit = resourceOfSocket.find(s);
      if (rit == resourceOfSocket.end()) return;
      auto sit = sessions.find(rit->second);
      if (sit == sessions.end()) return;
      auto& session = sit->second;
      session.bytesIn += n;
      bytesInWindow += n;

      const StreamMode mode = modeOfSocket[s];
      if (mode == StreamMode::Bidirectional) {
        for (const SRTSOCKET t : session.hub)
          if (t != s) targets.push_back(t);
      } else if (mode == StreamMode::Publish) {
        targets = session.requesters;
      }
    }

    // Send without mu held: libsrt must never be entered from under our lock.
    int64_t sent = 0;
    std::vector<SRTSOCKET> dead;
    for (const SRTSOCKET t : targets) {
      if (srt_sendmsg2(t, buf.data(), n, nullptr) == SRT_ERROR) {
        if (srt_getlasterror(nullptr) != SRT_EASYNCSND) dead.push_back(t);
      } else {
        sent += n;
      }
    }
    if (sent > 0) {
      std::lock_guard lock(mu);
      const auto rit = resourceOfSocket.find(s);
      if (rit != resourceOfSocket.end()) {
        const auto sit = sessions.find(rit->second);
        if (sit != sessions.end()) sit->second.bytesOut += sent;
      }
    }
    for (const SRTSOCKET t : dead) removeSocket(t);
  }

  void removeSocket(SRTSOCKET s) {
    {
      std::lock_guard lock(mu);
      const auto rit = resourceOfSocket.find(s);
      if (rit != resourceOfSocket.end()) {
        const auto sit = sessions.find(rit->second);
        if (sit != sessions.end()) {
          auto& session = sit->second;
          if (session.publisher == s) session.publisher = SRT_INVALID_SOCK;
          session.requesters.erase(
              std::remove(session.requesters.begin(), session.requesters.end(), s), session.requesters.end());
          session.hub.erase(std::remove(session.hub.begin(), session.hub.end(), s), session.hub.end());
          if (session.publisher == SRT_INVALID_SOCK && session.requesters.empty() && session.hub.empty())
            sessions.erase(sit);
        }
        resourceOfSocket.erase(s);
        modeOfSocket.erase(s);
      }
    }
    srt_close(s);  // srt_close unregisters it from the epoll
  }

  void measureInput() {
    std::lock_guard lock(mu);
    const auto now = Clock::now();
    const double seconds = std::chrono::duration<double>(now - windowStart).count();
    if (seconds > 0) totalMbps = static_cast<double>(bytesInWindow) * 8.0 / seconds / 1e6;
    bytesInWindow = 0;
    windowStart = now;
  }

  std::vector<SessionStats> snapshot() const {
    std::lock_guard lock(mu);
    std::vector<SessionStats> out;
    const auto now = Clock::now();
    for (const auto& [name, session] : sessions) {
      const double seconds = std::max(0.001, std::chrono::duration<double>(now - session.started).count());
      out.push_back(SessionStats{name, static_cast<double>(session.bytesIn) * 8.0 / seconds / 1e6,
                                 static_cast<double>(session.bytesOut) * 8.0 / seconds / 1e6,
                                 session.publisher != SRT_INVALID_SOCK ? 1 : 0,
                                 static_cast<int>(session.requesters.size() + session.hub.size())});
    }
    return out;
  }

  void printStats() const {
    for (const auto& s : snapshot()) {
      std::cout << "session=" << s.session << " in=" << s.mbpsIn << " out=" << s.mbpsOut
                << " pub=" << s.publishers << " req=" << s.requesters << "\n";
    }
    std::cout.flush();
  }
};

Relay::Relay(RelayConfig cfg) : impl_(std::make_unique<Impl>(std::move(cfg))) {}
Relay::~Relay() { impl_->stop(); }

void Relay::start() { impl_->start(); }
void Relay::stop() { impl_->stop(); }
std::vector<Relay::SessionStats> Relay::stats() const { return impl_->snapshot(); }

}  // namespace bmb