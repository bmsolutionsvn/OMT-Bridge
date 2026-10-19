#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "core/bridge_store.h"

namespace fs = std::filesystem;

static fs::path tempFile(const char* name) {
  // Unique per process: two concurrent test binaries (another checkout, ctest -j) must not
  // collide, and the absent-file case must not see a previous run's stale residue.
  static const std::string unique = std::to_string(std::random_device{}());
  const fs::path dir = fs::temp_directory_path() / ("ob-bridge-tests-" + unique);
  fs::create_directories(dir);
  return dir / name;
}

TEST_CASE("a bridge round-trips through JSON unchanged") {
  ob::Bridge b;
  b.name = "Hanoi Bridge";
  b.settings.defaultTarget = ob::OutputTarget::Ndi;
  b.settings.thumbnailIntervalMs = 1500;
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::OmtHx, .address = "PC (Cam)",
                                 .codec = ob::HxCodec::H264});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Ndi,
                                 .quality = ob::Quality::High, .alpha = ob::AlphaMode::Off,
                                 .name = "Cam 1"});

  const fs::path p = tempFile("round-trip.ombridge.json");
  ob::saveBridge(b, p);

  const auto loaded = ob::loadBridge(p);
  REQUIRE(loaded.has_value());
  CHECK(loaded->name == b.name);
  CHECK(loaded->sources == b.sources);
  CHECK(loaded->outputs == b.outputs);
  CHECK(loaded->settings == b.settings);
}

TEST_CASE("loading an absent file returns no value") {
  CHECK_FALSE(ob::loadBridge(tempFile("does-not-exist.ombridge.json")).has_value());
}

TEST_CASE("loading a file that is not JSON reports an error and returns no value") {
  const fs::path p = tempFile("garbage.ombridge.json");
  std::ofstream(p) << "this is not json";
  CHECK_FALSE(ob::loadBridge(p).has_value());
}

TEST_CASE("an unknown enum string in the file is rejected, not silently defaulted") {
  const fs::path p = tempFile("bad-enum.ombridge.json");
  std::ofstream(p) << R"({"name":"x","sources":[{"id":"s1","kind":"Whatever"}],"outputs":[]})";
  CHECK_FALSE(ob::loadBridge(p).has_value());
}

TEST_CASE("a file with no settings block loads with the struct defaults") {
  const fs::path p = tempFile("no-settings.ombridge.json");
  std::ofstream(p) << R"({"name":"x","sources":[],"outputs":[]})";
  const auto loaded = ob::loadBridge(p);
  REQUIRE(loaded.has_value());
  CHECK(loaded->settings == ob::BridgeSettings{});
}
