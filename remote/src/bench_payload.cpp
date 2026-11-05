#include "bench_payload.h"

#include <algorithm>
#include <cmath>

namespace bmb {
namespace {

void put64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

uint64_t get64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}

}  // namespace

void writeHeader(std::span<uint8_t> out, const PayloadHeader& h) {
  if (out.size() < kPayloadHeaderSize) return;
  put64(out.data() + 0, h.seq);
  put64(out.data() + 8, static_cast<uint64_t>(h.sendUs));
  for (int i = 0; i < 4; ++i) out[16 + i] = static_cast<uint8_t>(h.frame >> (8 * i));
  out[20] = static_cast<uint8_t>(h.part & 0xff);
  out[21] = static_cast<uint8_t>(h.part >> 8);
  out[22] = static_cast<uint8_t>(h.parts & 0xff);
  out[23] = static_cast<uint8_t>(h.parts >> 8);
  put64(out.data() + 24, h.reserved);
}

PayloadHeader readHeader(std::span<const uint8_t> in) {
  PayloadHeader h;
  if (in.size() < kPayloadHeaderSize) return h;
  h.seq = get64(in.data() + 0);
  h.sendUs = static_cast<int64_t>(get64(in.data() + 8));
  h.frame = static_cast<uint32_t>(in[16]) | static_cast<uint32_t>(in[17]) << 8 |
            static_cast<uint32_t>(in[18]) << 16 | static_cast<uint32_t>(in[19]) << 24;
  h.part = static_cast<uint16_t>(in[20] | in[21] << 8);
  h.parts = static_cast<uint16_t>(in[22] | in[23] << 8);
  h.reserved = get64(in.data() + 24);
  return h;
}

std::vector<int> frameSizes(int64_t bitrateBps, int fps, int keyEvery, double keyScale) {
  std::vector<int> sizes(static_cast<size_t>(std::max(1, keyEvery)), 0);
  if (fps <= 0 || keyEvery <= 0) return sizes;
  const double budget = static_cast<double>(bitrateBps) / 8.0 * keyEvery / fps;
  const double unit = budget / (keyScale + keyEvery - 1);
  const int normal = std::max(1, static_cast<int>(std::llround(unit)));
  sizes[0] = std::max(1, static_cast<int>(std::llround(unit * keyScale)));
  for (size_t i = 1; i < sizes.size(); ++i) sizes[i] = normal;
  return sizes;
}

void SeqTracker::mark(uint64_t seq) {
  const uint64_t i = seq % kWindow;
  bits_[i / 64] |= uint64_t{1} << (i % 64);
}

void SeqTracker::clear(uint64_t seq) {
  const uint64_t i = seq % kWindow;
  bits_[i / 64] &= ~(uint64_t{1} << (i % 64));
}

void SeqTracker::add(uint64_t seq) {
  if (has_ && seq <= highest_) {
    if (highest_ - seq >= kWindow) return;  // forgotten: older than the window
    const uint64_t i = seq % kWindow;
    if (bits_[i / 64] & (uint64_t{1} << (i % 64))) return;  // duplicate
    mark(seq);
    ++received_;
    return;
  }
  if (!has_) {
    std::fill(bits_.begin(), bits_.end(), 0);
  } else if (seq - highest_ >= kWindow) {
    std::fill(bits_.begin(), bits_.end(), 0);
  } else {
    for (uint64_t s = highest_ + 1; s <= seq; ++s) clear(s);
  }
  mark(seq);
  highest_ = seq;
  has_ = true;
  ++received_;
}

int64_t SeqTracker::gaps() const {
  if (!has_) return 0;
  const int64_t g = static_cast<int64_t>(highest_ + 1) - received_;
  return g < 0 ? 0 : g;
}

}  // namespace bmb