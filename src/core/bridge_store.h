#pragma once
// Bridge persistence (design 5.1, 6.6). One JSON file, owned by the UI. A load that fails
// returns no value; it never invents a default bridge.
#include <filesystem>
#include <optional>

#include "core/bridge_model.h"

namespace ob {

void saveBridge(const Bridge& bridge, const std::filesystem::path& path);
std::optional<Bridge> loadBridge(const std::filesystem::path& path);

}  // namespace ob
