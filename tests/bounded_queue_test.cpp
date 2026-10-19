#include <doctest/doctest.h>

#include <atomic>
#include <thread>

#include "core/bounded_queue.h"

using namespace std::chrono_literals;

TEST_CASE("push never blocks and drops the oldest when full") {
  ob::BoundedQueue<int> q(2);
  CHECK_FALSE(q.push(1));
  CHECK_FALSE(q.push(2));
  CHECK(q.push(3));  // dropped 1
  CHECK(q.dropped() == 1);
  CHECK(q.pop(0ms) == 2);
  CHECK(q.pop(0ms) == 3);
  CHECK_FALSE(q.pop(0ms).has_value());
}

TEST_CASE("close unblocks a waiting pop and reopen resumes") {
  ob::BoundedQueue<int> q(2);
  CHECK_FALSE(q.closed());
  q.close();
  CHECK(q.closed());
  CHECK_FALSE(q.push(1));
  CHECK_FALSE(q.pop(0ms).has_value());
  q.reopen();
  CHECK_FALSE(q.closed());
  CHECK_FALSE(q.push(1));
  CHECK(q.pop(0ms) == 1);
}

// The case above proves the *state*, not the *wakeup*: it closes before it pops, so nothing
// is ever parked. OutputRow::stop() (Task 11) relies on close() releasing a worker already
// blocked inside pop(), so that interleaving needs its own case with a real thread.
TEST_CASE("close releases a thread parked in pop") {
  ob::BoundedQueue<int> q(2);
  std::atomic<bool> returned = false;
  std::optional<int> got;
  std::thread t([&] {
    got = q.pop(5000ms);
    returned = true;
  });
  std::this_thread::sleep_for(50ms);
  CHECK_FALSE(returned.load());  // still parked: nothing pushed and not yet closed

  const auto start = std::chrono::steady_clock::now();
  q.close();
  t.join();
  const auto elapsed = std::chrono::steady_clock::now() - start;

  CHECK(returned.load());
  CHECK_FALSE(got.has_value());
  CHECK(q.closed());
  // pop's timeout is 5000 ms, so this passes only if close() *woke* the parked pop rather
  // than letting the timeout expire. Without it, a close() that forgot to notify would
  // still pass — 5 s slower. This is the assertion that makes the test about the wakeup.
  CHECK(elapsed < 1000ms);
}

TEST_CASE("push wakes a thread parked in pop") {
  ob::BoundedQueue<int> q(2);
  std::optional<int> got;
  std::thread t([&] { got = q.pop(5000ms); });
  std::this_thread::sleep_for(20ms);
  CHECK_FALSE(q.push(7));
  t.join();
  CHECK(got == 7);
}
