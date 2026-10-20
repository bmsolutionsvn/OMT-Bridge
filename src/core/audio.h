#pragma once
// One audio packet, planar float (design 10). Channel-major: all of channel 0's samples, then
// channel 1's, ... The bridge never resamples or mixes.
#include <cstdint>
#include <vector>

namespace ob {

struct AudioFormat {
  int sampleRate = 0;
  int channels = 0;
  bool operator==(const AudioFormat&) const = default;
};

struct AudioFrame {
  AudioFormat format;
  int samples = 0;            // per channel
  int64_t timestamp = 0;      // OMT units: 10,000,000 per second (NDI ticks are the same unit)
  std::vector<float> planar;  // channel 0's samples, then channel 1's, ...

  bool valid() const {
    return format.sampleRate > 0 && format.channels > 0 && samples > 0 &&
           planar.size() == static_cast<size_t>(samples) * static_cast<size_t>(format.channels);
  }
};

}  // namespace ob
