#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "engine/source_hub.h"

static std::shared_ptr<ob::Frame> makeFrame(int w, int h) {
  auto f = std::make_shared<ob::Frame>();
  f->format = ob::VideoFormat{w, h, 60, 1, false};
  f->uyvy.assign(f->lumaBytes(), 0x10);
  return f;
}

TEST_CASE("a hub runs when referenced or when its thumbnail is wanted, and stops otherwise") {
  ob::SourceHub hub("s1");
  CHECK_FALSE(hub.running());

  hub.addRef();
  CHECK(hub.running());
  hub.addRef();
  hub.release();
  CHECK(hub.running());  // one output left
  hub.release();
  CHECK_FALSE(hub.running());

  hub.setThumbnailWanted(true);
  CHECK(hub.running());
  hub.setThumbnailWanted(false);
  CHECK_FALSE(hub.running());
}

TEST_CASE("running changes are reported once per transition") {
  ob::SourceHub hub("s1");
  std::atomic<int> starts{0}, stops{0};
  hub.setOnRunningChanged([&](bool on) { (on ? starts : stops).fetch_add(1); });

  hub.addRef();
  hub.addRef();  // no second start
  hub.release();
  hub.release();

  CHECK(starts.load() == 1);
  CHECK(stops.load() == 1);
}

TEST_CASE("the latest frame is the newest published one") {
  ob::SourceHub hub("s1");
  // Assert on bools or through the pointee: doctest cannot stream a shared_ptr<const ob::Frame>
  // (it would need an operator<< for ob::Frame*, which does not exist — see the plan's rules).
  CHECK_FALSE(static_cast<bool>(hub.latest()));
  hub.storeFrame(makeFrame(4, 2));
  hub.storeFrame(makeFrame(8, 2));
  REQUIRE(hub.latest().get() != nullptr);
  CHECK(hub.latest()->format.width == 8);
}

TEST_CASE("a transparent alpha plane is kept and the stored frame stays valid") {
  ob::SourceHub hub("s1");
  auto f = std::make_shared<ob::Frame>();
  f->format = ob::VideoFormat{4, 2, 60, 1, true};
  f->uyvy.assign(f->lumaBytes() + f->alphaBytes(), 0xFF);
  f->premultiplied = true;
  f->uyvy[f->alphaOffset() + 1] = 128;  // one transparent pixel keeps alpha on
  hub.storeFrame(f);

  REQUIRE(hub.latest().get() != nullptr);
  CHECK(hub.latest()->format.alpha);
  CHECK(hub.latest()->premultiplied);
  CHECK(hub.latest()->valid());
}

// The class is the engine's synchronisation point, so its invariant gets a real interleaving:
// every false->true transition is matched by a true->false one, so starts must equal stops and
// the hub must end idle. This is the runnable check for the once-per-transition claim.
TEST_CASE("hammering addRef and release from several threads keeps starts and stops balanced") {
  ob::SourceHub hub("s1");
  std::atomic<int> starts{0}, stops{0};
  hub.setOnRunningChanged([&](bool on) { (on ? starts : stops).fetch_add(1); });

  constexpr int kThreads = 4;
  constexpr int kIters = 500;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < kIters; ++i) {
        hub.addRef();
        hub.release();
      }
    });
  }
  for (auto& th : threads) th.join();

  CHECK(hub.refs() == 0);
  CHECK_FALSE(hub.running());
  CHECK(starts.load() == stops.load());
}

// The gate itself is unit-tested; this pins that the hub actually applies it and that the
// published frame stays valid after the alpha plane is dropped.
TEST_CASE("the hub drops an opaque alpha plane after two seconds and keeps the frame valid") {
  auto now = std::chrono::steady_clock::now();
  ob::SourceHub hub("s1", [&] { return now; });

  const auto opaque = [] {
    auto f = std::make_shared<ob::Frame>();
    f->format = ob::VideoFormat{4, 2, 60, 1, true};
    f->uyvy.assign(f->lumaBytes() + f->alphaBytes(), 0xFF);
    f->premultiplied = true;
    return f;
  };

  hub.storeFrame(opaque());  // starts the window
  now += std::chrono::milliseconds(1500);
  hub.storeFrame(opaque());
  REQUIRE(hub.latest().get() != nullptr);
  CHECK(hub.latest()->format.alpha);  // under 2 s

  now += std::chrono::milliseconds(600);
  hub.storeFrame(opaque());  // 2.1 s of continuous opacity
  REQUIRE(hub.latest().get() != nullptr);
  CHECK_FALSE(hub.latest()->format.alpha);
  CHECK_FALSE(hub.latest()->premultiplied);
  CHECK(hub.latest()->valid());
  CHECK(hub.latest()->uyvy.size() == hub.latest()->lumaBytes());
}
