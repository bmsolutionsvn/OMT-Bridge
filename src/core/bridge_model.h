#pragma once
// The three objects of the bridge (design 5). Bridge is level 2; Source and Output are level 3.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/signal_state.h"

namespace ob {

enum class SourceKind { Omt, OmtHx, Ndi };
enum class OutputTarget { Omt, OmtHx, Ndi };
enum class HxCodec { Hevc, H264 };
enum class Quality { Low, Medium, High };
enum class AlphaMode { Auto, Off };

std::string_view toString(SourceKind k);
std::string_view toString(OutputTarget t);
std::string_view toString(HxCodec c);
std::string_view toString(Quality q);
std::string_view toString(AlphaMode a);

// The wire names, and their one home. kind/target/codec take their proper spelling ("OMT",
// "OMT-HX", "NDI", "HEVC", "H.264"); quality and alpha follow design 5.2/5.3's field listing
// ("Low"/"Medium"/"High", "Auto"/"Off"). These differ in case from `state` ("live"), which is
// lowercase only because it is lifted verbatim from BreadCamera's shipped wire contract
// (Task 6) and must stay diffable.
//
// The parsers below compare against toString(...) rather than re-listing the literals, so each
// name exists exactly once. The JSON store (Task 9) and the command router (Task 13) MUST use
// these — never their own name tables, which is how the protocol would otherwise end up writing
// "OMT" in a state reply while rejecting it in an add-source.
std::optional<SourceKind> sourceKindFromString(std::string_view s);
std::optional<OutputTarget> outputTargetFromString(std::string_view s);
std::optional<HxCodec> hxCodecFromString(std::string_view s);
std::optional<Quality> qualityFromString(std::string_view s);
std::optional<AlphaMode> alphaModeFromString(std::string_view s);

// What a source is sending, once known. Kept separate from core/frame.h's VideoFormat so
// core/ stays free of the omthx headers; the adapters copy one into the other.
struct VideoFormatView {
  int width = 0;
  int height = 0;
  int fpsN = 60;
  int fpsD = 1;
  bool alpha = false;
  bool known = false;
  bool operator==(const VideoFormatView&) const = default;
};

struct Source {
  std::string id;
  SourceKind kind = SourceKind::Omt;
  std::string address;  // OMT: "HOSTNAME (name)"  ·  NDI: the NDI source name
  SignalState state = SignalState::Ready;  // runtime liveness: NOT persisted (Task 9)
  VideoFormatView format;                  // runtime: NOT persisted
  std::optional<HxCodec> codec;  // OMT-HX only

  // Equality is CONFIGURATION equality: `state` and `format` are live values, not saved config,
  // so a source that goes Live still equals its on-disk form. The JSON store round-trip (Task 9)
  // compares exactly this, and no future diff-based apply() may read a liveness change as an
  // edit and restart an adapter.
  bool operator==(const Source& o) const {
    return id == o.id && kind == o.kind && address == o.address && codec == o.codec;
  }
};

struct Output {
  std::string id;
  std::string sourceId;  // any Source, or empty
  OutputTarget target = OutputTarget::Omt;
  HxCodec codec = HxCodec::Hevc;
  Quality quality = Quality::Medium;
  AlphaMode alpha = AlphaMode::Auto;
  std::string name;
  SignalState state = SignalState::Ready;  // runtime liveness: NOT persisted

  // Configuration equality (see Source): `state` is live liveness, not saved config.
  bool operator==(const Output& o) const {
    return id == o.id && sourceId == o.sourceId && target == o.target && codec == o.codec &&
           quality == o.quality && alpha == o.alpha && name == o.name;
  }
};

struct BridgeSettings {
  OutputTarget defaultTarget = OutputTarget::OmtHx;
  Quality defaultQuality = Quality::Medium;
  int thumbnailIntervalMs = 2000;
  bool startEngineWithApp = false;
  bool operator==(const BridgeSettings&) const = default;
};

struct Bridge {
  std::string name;
  std::vector<Source> sources;
  std::vector<Output> outputs;
  BridgeSettings settings;

  Source* findSource(const std::string& id);
  const Source* findSource(const std::string& id) const;
  Output* findOutput(const std::string& id);
  const Output* findOutput(const std::string& id) const;

  // N x M: how many outputs read this source.
  size_t outputsFor(const std::string& sourceId) const;

  // "<Bridge.name> (<Output.name>)", the name a remote app sees (design 5.3).
  std::string advertisedName(const Output& o) const;
};

}  // namespace ob
