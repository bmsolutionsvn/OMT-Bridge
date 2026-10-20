#include <doctest/doctest.h>
#include "core/audio.h"

TEST_CASE("audio validity keeps any rate and channel count") {
  ob::AudioFrame f;
  f.format = ob::AudioFormat{44100, 6};
  f.samples = 4;
  f.planar.assign(24, 0.0f);
  CHECK(f.valid());

  f.format = ob::AudioFormat{48000, 2};
  f.planar.assign(8, 0.0f);
  CHECK(f.valid());

  f.format = ob::AudioFormat{0, 2};
  CHECK_FALSE(f.valid());

  f.format = ob::AudioFormat{48000, 0};
  f.samples = 4;
  f.planar.assign(8, 0.0f);
  CHECK_FALSE(f.valid());

  f.format = ob::AudioFormat{48000, 2};
  f.samples = 5;              // planar still holds 8 floats
  CHECK_FALSE(f.valid());

  f.samples = 4;
  f.planar.pop_back();
  CHECK_FALSE(f.valid());
}
