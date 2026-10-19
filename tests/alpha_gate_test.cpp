#include <doctest/doctest.h>

#include <chrono>
#include <vector>

#include "core/alpha_gate.h"

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

TEST_CASE("no alpha means nothing to drop") {
  ob::AlphaGate g;
  const auto t0 = Clock::now();
  CHECK_FALSE(g.shouldDrop(false, nullptr, 0, t0));
}

TEST_CASE("one sub-255 pixel turns alpha on immediately and keeps it on") {
  ob::AlphaGate g;
  const auto t0 = Clock::now();
  std::vector<uint8_t> a(16, 255);
  a[7] = 254;
  CHECK_FALSE(g.shouldDrop(true, a.data(), a.size(), t0));       // not opaque: keep it
  CHECK_FALSE(g.shouldDrop(true, a.data(), a.size(), t0 + 3s));  // still not opaque
}

TEST_CASE("alpha is dropped only after 2 s of continuous opacity") {
  ob::AlphaGate g;
  const auto t0 = Clock::now();
  std::vector<uint8_t> opaque(16, 255);
  CHECK_FALSE(g.shouldDrop(true, opaque.data(), opaque.size(), t0));       // starts the clock
  CHECK_FALSE(g.shouldDrop(true, opaque.data(), opaque.size(), t0 + 1s));  // under 2 s
  CHECK(g.shouldDrop(true, opaque.data(), opaque.size(), t0 + 2s));        // 2 s reached
}

TEST_CASE("a single transparent frame resets the hysteresis") {
  ob::AlphaGate g;
  const auto t0 = Clock::now();
  std::vector<uint8_t> opaque(16, 255);
  std::vector<uint8_t> keyed(16, 128);
  CHECK_FALSE(g.shouldDrop(true, opaque.data(), opaque.size(), t0));
  CHECK_FALSE(g.shouldDrop(true, keyed.data(), keyed.size(), t0 + 1s));   // resets
  CHECK_FALSE(g.shouldDrop(true, opaque.data(), opaque.size(), t0 + 2s)); // clock restarted
  CHECK(g.shouldDrop(true, opaque.data(), opaque.size(), t0 + 4s));
}
