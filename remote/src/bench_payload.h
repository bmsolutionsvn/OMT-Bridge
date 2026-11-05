#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace bmb {

// Header at the start of every bench SRT message; 32 bytes, little-endian.
struct PayloadHeader {
  uint64_t seq = 0;      // per stream, from 0
  int64_t sendUs = 0;    // sender's steady clock
  uint32_t frame = 0;
  uint16_t part = 0;
  uint16_t parts = 1;
  uint64_t reserved = 0;
};

inline constexpr size_t kPayloadHeaderSize = 32;

void writeHeader(std::span<uint8_t>, const PayloadHeader&);
PayloadHeader readHeader(std::span<const uint8_t>);

// One GOP of `keyEvery` frame sizes in bytes: index 0 is keyScale x the others, and the
// sum is bitrateBps / 8 * keyEvery / fps.
std::vector<int> frameSizes(int64_t bitrateBps, int fps, int keyEvery, double keyScale);

// Counts distinct sequence numbers in the last 64K window; gaps = highest + 1 - received.
class SeqTracker {
 public:
  void add(uint64_t seq);
  int64_t gaps() const;
  int64_t received() const { return received_; }

 private:
  static constexpr uint64_t kWindow = 1 << 16;
  void mark(uint64_t seq);
  void clear(uint64_t seq);

  std::vector<uint64_t> bits_ = std::vector<uint64_t>(kWindow / 64, 0);
  bool has_ = false;
  uint64_t highest_ = 0;
  int64_t received_ = 0;
};

}  // namespace bmb