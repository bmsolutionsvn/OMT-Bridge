#include <doctest/doctest.h>

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
