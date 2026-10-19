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

// The policy's headline behaviour: five quick restarts inside the window, then one per 10 s.
TEST_CASE("rapid relaunches are throttled to the slow interval once the budget is spent") {
  FakeLauncher l;  // dead throughout
  ob::Supervisor sup(l);
  const auto t0 = Clock::now();

  for (int i = 0; i < 5; ++i) {
    sup.poll(t0 + std::chrono::seconds(i));  // 1 s apart, inside 60 s
    l.running = false;                       // launch() marks it running; it dies again at once
  }
  CHECK(l.launched == 5);
  CHECK(sup.crashLooping(t0 + std::chrono::seconds(5)));

  sup.poll(t0 + std::chrono::seconds(6));  // inside the window and the slow interval: throttled
  CHECK(l.launched == 5);

  sup.poll(t0 + std::chrono::seconds(15));  // 11 s since the last attempt: one more is allowed
  CHECK(l.launched == 6);
}
