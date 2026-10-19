#include "core/bridge_store.h"

#include <fstream>
#include <stdexcept>
#include <system_error>
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

  // Write to a sibling temp file and replace it in one rename, so a failure can never truncate
  // the good file. The write is CHECKED: otherwise a full disk or an unwritable directory would
  // leave a truncated or empty file renamed over the user's configuration. `path += ".tmp"`
  // appends to the filename with no narrow-encoding round-trip through path.string().
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
  out << j.dump(2);
  out.flush();
  if (!out) {
    out.close();
    std::error_code ignored;
    std::filesystem::remove(tmp, ignored);  // never leave a stray .tmp behind
    throw std::runtime_error("bridge save failed: could not write " + tmp.string());
  }
  out.close();
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) throw std::runtime_error("bridge save failed: " + ec.message());
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
      // Defaults come from BridgeSettings itself, so no enum wire-name literal is re-spelled
      // here: the names live only in bridge_model. An absent key keeps the struct default; a key
      // present but unparseable rejects the whole file.
      b.settings = BridgeSettings{};
      if (s.contains("defaultTarget")) {
        auto v = outputTargetFromString(s.value("defaultTarget", std::string{}));
        if (!v) return std::nullopt;
        b.settings.defaultTarget = *v;
      }
      if (s.contains("defaultQuality")) {
        auto v = qualityFromString(s.value("defaultQuality", std::string{}));
        if (!v) return std::nullopt;
        b.settings.defaultQuality = *v;
      }
      b.settings.thumbnailIntervalMs = s.value("thumbnailIntervalMs", b.settings.thumbnailIntervalMs);
      b.settings.startEngineWithApp = s.value("startEngineWithApp", b.settings.startEngineWithApp);
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
      // Absent optional keys keep Output's own defaults (again, no wire-name literals here).
      if (jo.contains("codec")) {
        auto codec = hxCodecFromString(jo.value("codec", std::string{}));
        if (!codec) return std::nullopt;
        o.codec = *codec;
      }
      if (jo.contains("quality")) {
        auto quality = qualityFromString(jo.value("quality", std::string{}));
        if (!quality) return std::nullopt;
        o.quality = *quality;
      }
      if (jo.contains("alpha")) {
        auto alpha = alphaModeFromString(jo.value("alpha", std::string{}));
        if (!alpha) return std::nullopt;
        o.alpha = *alpha;
      }
      o.name = jo.value("name", std::string{});
      b.outputs.push_back(std::move(o));
    }

    return b;
  } catch (const json::exception&) {
    return std::nullopt;
  }
}

}  // namespace ob
