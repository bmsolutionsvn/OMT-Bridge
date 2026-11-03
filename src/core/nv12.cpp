#include "core/nv12.h"

#include <cstddef>

#include "core/frame.h"

namespace ob {
namespace {

// Round half away from zero, as std::lround does for the two tables below, without its non-constexpr.
constexpr int roundedDiv(int num, int den) { return num >= 0 ? (num + den / 2) / den : -((-num + den / 2) / den); }

struct LumaTable {
  uint8_t v[256] = {};
  constexpr LumaTable() {
    for (int i = 0; i < 256; ++i) v[i] = static_cast<uint8_t>(16 + roundedDiv(i * 219, 255));
  }
};

struct ChromaTable {
  uint8_t v[256] = {};
  constexpr ChromaTable() {
    for (int i = 0; i < 256; ++i) v[i] = static_cast<uint8_t>(128 + roundedDiv((i - 128) * 224, 255));
  }
};

constexpr LumaTable kLuma;
constexpr ChromaTable kChroma;

}  // namespace

void nv12FullToLimited(std::span<const uint8_t> full, int width, int height, std::vector<uint8_t>& out) {
  const size_t yBytes = static_cast<size_t>(width) * static_cast<size_t>(height);
  const size_t uvBytes = yBytes / 2;
  out.resize(yBytes + uvBytes);
  for (size_t i = 0; i < yBytes; ++i) out[i] = kLuma.v[full[i]];
  for (size_t i = 0; i < uvBytes; ++i) out[yBytes + i] = kChroma.v[full[yBytes + i]];
}

std::shared_ptr<const std::vector<uint8_t>> Frame::limitedNv12() const {
  if (layout != Layout::Nv12) return nullptr;
  const std::shared_ptr<LimitedNv12> state = limited_;
  std::call_once(state->once, [this, state] {
    auto bytes = std::make_shared<std::vector<uint8_t>>();
    nv12FullToLimited(nv12, format.width, format.height, *bytes);
    state->bytes = std::move(bytes);
  });
  return state->bytes;
}

}  // namespace ob
