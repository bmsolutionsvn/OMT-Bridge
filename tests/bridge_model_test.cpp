#include <doctest/doctest.h>

#include <string>

#include "core/bridge_model.h"

TEST_CASE("a new bridge has a default name and no sources or outputs") {
  ob::Bridge b;
  CHECK(b.name.empty());
  CHECK(b.sources.empty());
  CHECK(b.outputs.empty());
}

TEST_CASE("findSource and findOutput return nullptr for an unknown id") {
  ob::Bridge b;
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Omt, .address = "PC (Cam)"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Omt});
  CHECK(b.findSource("s1") != nullptr);
  CHECK(b.findSource("nope") == nullptr);
  CHECK(b.findOutput("o1") != nullptr);
  CHECK(b.findOutput("nope") == nullptr);
}

TEST_CASE("outputsFor counts every output that reads one source (N x M)") {
  ob::Bridge b;
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Ndi, .address = "CAM"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::OmtHx});
  b.outputs.push_back(ob::Output{.id = "o2", .sourceId = "s1", .target = ob::OutputTarget::Ndi});
  b.outputs.push_back(ob::Output{.id = "o3", .sourceId = "s2", .target = ob::OutputTarget::Omt});
  CHECK(b.outputsFor("s1") == 2u);
  CHECK(b.outputsFor("s2") == 1u);
  CHECK(b.outputsFor("none") == 0u);
}

TEST_CASE("the advertised name joins the bridge and row names") {
  ob::Bridge b;
  b.name = "Hanoi Bridge";
  ob::Output o{.id = "o1", .sourceId = "", .target = ob::OutputTarget::Omt, .name = "Cam 1"};
  CHECK(b.advertisedName(o) == "Hanoi Bridge (Cam 1)");
}

TEST_CASE("advertisedName falls back to the row name when the bridge is unnamed") {
  ob::Bridge b;
  ob::Output o{.id = "o1", .sourceId = "", .target = ob::OutputTarget::Omt, .name = "Cam 1"};
  CHECK(b.advertisedName(o) == "Cam 1");
}

// Pins every wire name. `toString` returning a string_view compared to a literal is the same
// doctest decomposition trap Task 6 hit, hence the std::string(...) wrappers.
TEST_CASE("the enums serialise to their wire names") {
  CHECK(std::string(ob::toString(ob::SourceKind::Omt)) == "OMT");
  CHECK(std::string(ob::toString(ob::SourceKind::OmtHx)) == "OMT-HX");
  CHECK(std::string(ob::toString(ob::SourceKind::Ndi)) == "NDI");
  CHECK(std::string(ob::toString(ob::OutputTarget::Omt)) == "OMT");
  CHECK(std::string(ob::toString(ob::OutputTarget::OmtHx)) == "OMT-HX");
  CHECK(std::string(ob::toString(ob::OutputTarget::Ndi)) == "NDI");
  CHECK(std::string(ob::toString(ob::HxCodec::Hevc)) == "HEVC");
  CHECK(std::string(ob::toString(ob::HxCodec::H264)) == "H.264");
  CHECK(std::string(ob::toString(ob::Quality::Low)) == "Low");
  CHECK(std::string(ob::toString(ob::Quality::Medium)) == "Medium");
  CHECK(std::string(ob::toString(ob::Quality::High)) == "High");
  CHECK(std::string(ob::toString(ob::AlphaMode::Auto)) == "Auto");
  CHECK(std::string(ob::toString(ob::AlphaMode::Off)) == "Off");
}
