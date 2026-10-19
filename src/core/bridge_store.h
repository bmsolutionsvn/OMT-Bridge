#pragma once
// Bridge persistence (design 5.1, 6.6). One JSON file, owned by the UI.
//
// loadBridge: a load that fails returns no value; it never invents a default bridge. It cannot
// distinguish a missing file from a corrupt one (both are no value) — Task 12 must decide
// deliberately whether that is acceptable at engine start.
//
// saveBridge: writes atomically (sibling .tmp, then one rename) and THROWS std::runtime_error if
// the write or the rename fails, so a failed save can never silently replace a good file.
#include <filesystem>
#include <optional>

#include "core/bridge_model.h"

namespace ob {

void saveBridge(const Bridge& bridge, const std::filesystem::path& path);
std::optional<Bridge> loadBridge(const std::filesystem::path& path);

}  // namespace ob
