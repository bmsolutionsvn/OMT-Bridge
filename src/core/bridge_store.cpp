#include "core/bridge_store.h"

#include <fstream>
#include <utility>

#include <nlohmann/json.hpp>

using nlohmann::json;

namespace ob {

// No name tables here. The wire names live once, in core/bridge_model: toString() writes and the
// *FromString() parsers read, so the store cannot drift from the enum contract (and from the
// command router, which uses the same functions).
void saveBridge(const Bridge& b, const std::filesystem::path& path) {
  json j;
  j["name"] = b.name;
  j["settings"] = {{"defaultTarget", std::string(toString(b.settings.defaultTarget))},
                   {"defaultQuality", std::string(toString(b.settings.defaultQuality))},
                   {"thumbnailIntervalMs", b.settings.thumbnailIntervalMs},
                   {"startEngineWithApp", b.settings.startEngineWithApp}};

  j["sources"] = json::array();
  for (const Source& s : b.sources) {
    json js{{"id", s.id}, {"kind", std::string(toString(s.kind))}, {"address", s.address}};
    if (s.codec.has_value()) js["codec"] = std::string(toString(*s.codec));
    j["sources"].push_back(std::move(js));
  }

  j["outputs"] = json::array();
  for (const Output& o : b.outputs) {
    j["outputs"].push_back(json{{"id", o.id},
                                {"sourceId", o.sourceId},
                                {"target", std::string(toString(o.target))},
                                {"codec", std::string(toString(o.codec))},
                                {"quality", std::string(toString(o.quality))},
                                {"alpha", std::string(toString(o.alpha))},
                                {"name", o.name}});
  }

  const std::filesystem::path tmp = path.string() + ".tmp";
  std::ofstream(tmp, std::ios::binary | std::ios::trunc) << j.dump(2);
  std::filesystem::rename(tmp, path);
}

std::optional<Bridge> loadBridge(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;

  json j;
  try {
    j = json::parse(in);
  } catch (const json::exception&) {
    return std::nullopt;
  }

  try {
    Bridge b;
    b.name = j.value("name", std::string{});

    if (j.contains("settings")) {
      const auto& s = j["settings"];
      if (auto v = outputTargetFromString(s.value("defaultTarget", std::string{"OMT-HX"})))
        b.settings.defaultTarget = *v;
      else
        return std::nullopt;
      if (auto v = qualityFromString(s.value("defaultQuality", std::string{"Medium"})))
        b.settings.defaultQuality = *v;
      else
        return std::nullopt;
      b.settings.thumbnailIntervalMs = s.value("thumbnailIntervalMs", 2000);
      b.settings.startEngineWithApp = s.value("startEngineWithApp", false);
    }

    for (const auto& js : j.value("sources", json::array())) {
      Source s;
      s.id = js.value("id", std::string{});
      auto kind = sourceKindFromString(js.value("kind", std::string{}));
      if (!kind) return std::nullopt;
      s.kind = *kind;
      s.address = js.value("address", std::string{});
      if (js.contains("codec")) {
        auto c = hxCodecFromString(js.value("codec", std::string{}));
        if (!c) return std::nullopt;
        s.codec = *c;
      }
      b.sources.push_back(std::move(s));
    }

    for (const auto& jo : j.value("outputs", json::array())) {
      Output o;
      o.id = jo.value("id", std::string{});
      o.sourceId = jo.value("sourceId", std::string{});
      auto target = outputTargetFromString(jo.value("target", std::string{}));
      if (!target) return std::nullopt;
      o.target = *target;
      auto codec = hxCodecFromString(jo.value("codec", std::string{"HEVC"}));
      if (!codec) return std::nullopt;
      o.codec = *codec;
      auto quality = qualityFromString(jo.value("quality", std::string{"Medium"}));
      if (!quality) return std::nullopt;
      o.quality = *quality;
      auto alpha = alphaModeFromString(jo.value("alpha", std::string{"Auto"}));
      if (!alpha) return std::nullopt;
      o.alpha = *alpha;
      o.name = jo.value("name", std::string{});
      b.outputs.push_back(std::move(o));
    }

    return b;
  } catch (const json::exception&) {
    return std::nullopt;
  }
}

}  // namespace ob
