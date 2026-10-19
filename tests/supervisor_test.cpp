#include <doctest/doctest.h>

#include "shell/supervisor.h"

using Clock = ob::Supervisor::Clock;

struct FakeLauncher : ob::EngineLauncher {
  bool running = false;
  int launched = 0, killed = 0;
  bool isRunning() override { return running; }
  void launch() override { ++launched; running = true; }
  void kill() override { ++killed; running = false; }
};

TEST_CASE("the supervisor launches a dead engine and relaunches it while it stays dead") {
  FakeLauncher l;  // not running
  ob::Supervisor sup(l);
  const auto t0 = Clock::now();

  sup.poll(t0);
  CHECK(l.launched == 1);

  l.running = false;  // the engine exited: launch() marks it running, so clear it to stay dead
  sup.poll(t0 + std::chrono::seconds(1));  // a dead engine is relaunched at once, not delayed by grace
  CHECK(l.launched == 2);
}

TEST_CASE("the startup grace protects a slow-starting engine from the heartbeat timeout") {
  FakeLauncher l;
  l.running = true;  // launched, but it has not sent a heartbeat yet
  ob::Supervisor sup(l);
  const auto t0 = Clock::now();

  sup.poll(t0);                             // sets the baseline
  sup.poll(t0 + std::chrono::seconds(5));   // 5 s > the 3 s heartbeat timeout, but < the 8 s grace
  CHECK(l.killed == 0);

  sup.poll(t0 + std::chrono::seconds(9));   // past the grace, heartbeat still stale
  CHECK(l.killed == 1);
}

TEST_CASE("a fresh heartbeat keeps a running engine from being killed") {
  FakeLauncher l;
  l.running = true;
  ob::Supervisor sup(l);
  const auto t0 = Clock::now();

  sup.poll(t0);
  sup.heartbeat(t0 + std::chrono::seconds(8));
  sup.poll(t0 + std::chrono::seconds(9));  // past the grace, but the beat is only 1 s old
  CHECK(l.killed == 0);
}
