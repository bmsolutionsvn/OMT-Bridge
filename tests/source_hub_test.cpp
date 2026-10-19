#include <doctest/doctest.h>

#include <atomic>
#include <memory>

#include "engine/source_hub.h"

static std::shared_ptr<const ob::Frame> makeFrame(int w, int h) {
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
  hub.publish(makeFrame(4, 2));
  hub.publish(makeFrame(8, 2));
  REQUIRE(hub.latest().get() != nullptr);
  CHECK(hub.latest()->format.width == 8);
}
