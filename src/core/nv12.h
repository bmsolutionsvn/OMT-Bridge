#pragma once
// Full-range BT.709 NV12 to the limited range NDI and plain OMT take, by table.
#include <cstdint>
#include <span>
#include <vector>

namespace ob {

// `full` is width * height luma bytes then width * height / 2 interleaved chroma bytes; `out` gets the
// same layout, limited range. Endpoints: Y 0->16, 255->235; C 0->16, 128->128, 255->240.
void nv12FullToLimited(std::span<const uint8_t> full, int width, int height, std::vector<uint8_t>& out);

}  // namespace ob
