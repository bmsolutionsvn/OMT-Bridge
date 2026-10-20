#include "engine/command_router.h"

#include <algorithm>
#include <exception>
#include <optional>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

using nlohmann::json;

namespace ob {
namespace {

std::string refuse(const std::string& message) {
  return json{{"ok", false}, {"error", message}}.dump();
}

}  // namespace

void CommandRouter::setBridge(Bridge b) {
  bridge_ = std::move(b);
  nextSourceId_ = 1;
  nextOutputId_ = 1;
  // Seed each counter past its own ids in the loaded model: a duplicate id would make
  // findSource/remove-source act on the wrong row, and make a state reply list two rows under one.
  const auto seed = [](const std::string& id, char prefix, uint64_t& next) {
    if (id.size() < 2 || id[0] != prefix) return;
    try {
      const uint64_t n = std::stoull(id.substr(1));
      if (n >= next) next = n + 1;
    } catch (const std::exception&) {
      // Not a generated id (e.g. hand-edited); leave the counter alone.
    }
  };
  for (const Source& s : bridge_.sources) seed(s.id, 's', nextSourceId_);
  for (const Output& o : bridge_.outputs) seed(o.id, 'o', nextOutputId_);
}

void CommandRouter::setRuntime(SourceRuntimeFn sources, OutputRuntimeFn outputs) {
  sourceRuntime_ = std::move(sources);
  outputRuntime_ = std::move(outputs);
}

void CommandRouter::setDiscovery(DiscoveryFn discovery) { discovery_ = std::move(discovery); }

std::string CommandRouter::handle(const ClientId&, const std::string& text) {
  // ONE try around the whole body. Every field read below (j.value(...), j["id"], ...) throws
  // nlohmann's type_error.302 on a wrong-typed value — `{"cmd":"add-output","sourceId":42}` — and
  // this runs on a control-server thread, where an escaping exception would call std::terminate.
  // The router's contract is that it never throws to its caller; this is what makes that true for
  // malformed as well as unparseable input.
  try {
    json j = json::parse(text);
    if (!j.is_object() || !j.contains("cmd") || !j["cmd"].is_string())
      return refuse("every command needs a text \"cmd\"");

    const std::string cmd = j["cmd"];

    if (cmd == "state") {
      json sources = json::array();
      for (const Source& s : bridge_.sources) {
        json row = {{"id", s.id},
                    {"kind", std::string(toString(s.kind))},
                    {"address", s.address}};
        // Runtime keys appear only when the engine has a row for this id: a loaded-but-not-running
        // model must not claim a state it never measured.
        if (sourceRuntime_) {
          if (const auto rt = sourceRuntime_(s.id)) {
            row["state"] = std::string(toString(rt->state));
            if (rt->format.known) {
              row["width"] = rt->format.width;
              row["height"] = rt->format.height;
              row["fpsN"] = rt->format.fpsN;
              row["fpsD"] = rt->format.fpsD;
              row["alpha"] = rt->format.alpha;
            }
          }
        }
        sources.push_back(std::move(row));
      }
      json outputs = json::array();
      for (const Output& o : bridge_.outputs) {
        json row = {{"id", o.id},
                    {"sourceId", o.sourceId},
                    {"target", std::string(toString(o.target))},
                    {"name", o.name},
                    {"advertised", bridge_.advertisedName(o)}};
        if (outputRuntime_) {
          if (const auto rt = outputRuntime_(o.id)) {
            row["state"] = std::string(toString(rt->state));
            row["note"] = rt->note;
            row["delivered"] = rt->delivered;
            row["dropped"] = rt->dropped;
            row["audioDelivered"] = rt->audioDelivered;
            row["fps"] = rt->fps;
            row["mbps"] = rt->mbps;
            row["connections"] = rt->connections;
          }
        }
        outputs.push_back(std::move(row));
      }
      return json{{"ok", true},
                  {"name", bridge_.name},
                  {"sources", sources},
                  {"outputs", outputs},
                  {"settings",
                   {{"defaultTarget", std::string(toString(bridge_.settings.defaultTarget))},
                    {"defaultQuality", std::string(toString(bridge_.settings.defaultQuality))},
                    {"thumbnailIntervalMs", bridge_.settings.thumbnailIntervalMs},
                    {"startEngineWithApp", bridge_.settings.startEngineWithApp}}}}
          .dump();
    }

    if (cmd == "add-source") {
      if (!j.contains("address") || !j["address"].is_string())
        return refuse("add-source needs a text \"address\"");
      if (!j.contains("kind") || !j["kind"].is_string())
        return refuse("add-source needs a text \"kind\"");
      // Wire names come from core/bridge_model — the same table a state reply writes — so an
      // unknown string is an error rather than a silent default.
      const auto kind = sourceKindFromString(j["kind"]);
      if (!kind) return refuse("add-source: unknown \"kind\": " + j["kind"].get<std::string>());

      Source s;
      s.id = "s" + std::to_string(nextSourceId_++);
      s.kind = *kind;
      s.address = j["address"];
      bridge_.sources.push_back(std::move(s));
      return json{{"ok", true}, {"id", bridge_.sources.back().id}}.dump();
    }

    if (cmd == "remove-source") {
      if (!j.contains("id") || !j["id"].is_string()) return refuse("remove-source needs a text \"id\"");
      const std::string id = j["id"];
      if (bridge_.findSource(id) == nullptr) return refuse("remove-source: no such source: " + id);
      for (Output& o : bridge_.outputs)
        if (o.sourceId == id) o.sourceId.clear();
      bridge_.sources.erase(
          std::remove_if(bridge_.sources.begin(), bridge_.sources.end(),
                         [&](const Source& s) { return s.id == id; }),
          bridge_.sources.end());
      return json{{"ok", true}}.dump();
    }

    if (cmd == "add-output") {
      if (j.contains("sourceId") && !j["sourceId"].is_string())
        return refuse("add-output: \"sourceId\" must be text");
      if (j.contains("name") && !j["name"].is_string())
        return refuse("add-output: \"name\" must be text");
      Output o;
      o.id = "o" + std::to_string(nextOutputId_++);
      o.sourceId = j.value("sourceId", std::string{});
      o.name = j.value("name", std::string{"Output " + o.id});
      bridge_.outputs.push_back(std::move(o));
      return json{{"ok", true}, {"id", bridge_.outputs.back().id}}.dump();
    }

    if (cmd == "set-output-source") {
      if (!j.contains("id") || !j["id"].is_string()) return refuse("set-output-source needs a text \"id\"");
      if (j.contains("sourceId") && !j["sourceId"].is_string())
        return refuse("set-output-source: \"sourceId\" must be text");
      Output* o = bridge_.findOutput(j["id"]);
      if (o == nullptr) return refuse("set-output-source: no such output: " + j["id"].get<std::string>());
      const std::string sid = j.value("sourceId", std::string{});
      if (!sid.empty() && bridge_.findSource(sid) == nullptr)
        return refuse("set-output-source: no such source: " + sid);
      o->sourceId = sid;
      return json{{"ok", true}}.dump();
    }

    if (cmd == "set-output") {
      if (!j.contains("id") || !j["id"].is_string()) return refuse("set-output needs a text \"id\"");
      const std::string id = j["id"];
      Output* o = bridge_.findOutput(id);
      if (o == nullptr) return refuse("set-output: no such output: " + id);

      // Validate every field before assigning any: a command that names a bad target must change
      // nothing, not half of the row.
      std::optional<std::string> sourceId;
      if (j.contains("sourceId")) {
        if (!j["sourceId"].is_string()) return refuse("set-output: \"sourceId\" must be text");
        const std::string sid = j["sourceId"];
        if (!sid.empty() && bridge_.findSource(sid) == nullptr)
          return refuse("set-output: no such source: " + sid);
        sourceId = sid;
      }
      std::optional<OutputTarget> target;
      if (j.contains("target")) {
        if (!j["target"].is_string()) return refuse("set-output: \"target\" must be text");
        const auto parsed = outputTargetFromString(j["target"].get<std::string>());
        if (!parsed) return refuse("set-output: unknown \"target\": " + j["target"].get<std::string>());
        target = *parsed;
      }
      std::optional<HxCodec> codec;
      if (j.contains("codec")) {
        if (!j["codec"].is_string()) return refuse("set-output: \"codec\" must be text");
        const auto parsed = hxCodecFromString(j["codec"].get<std::string>());
        if (!parsed) return refuse("set-output: unknown \"codec\": " + j["codec"].get<std::string>());
        codec = *parsed;
      }
      std::optional<Quality> quality;
      if (j.contains("quality")) {
        if (!j["quality"].is_string()) return refuse("set-output: \"quality\" must be text");
        const auto parsed = qualityFromString(j["quality"].get<std::string>());
        if (!parsed) return refuse("set-output: unknown \"quality\": " + j["quality"].get<std::string>());
        quality = *parsed;
      }
      std::optional<AlphaMode> alpha;
      if (j.contains("alpha")) {
        if (!j["alpha"].is_string()) return refuse("set-output: \"alpha\" must be text");
        const auto parsed = alphaModeFromString(j["alpha"].get<std::string>());
        if (!parsed) return refuse("set-output: unknown \"alpha\": " + j["alpha"].get<std::string>());
        alpha = *parsed;
      }
      std::optional<std::string> name;
      if (j.contains("name")) {
        if (!j["name"].is_string()) return refuse("set-output: \"name\" must be text");
        const std::string parsed = j["name"];
        if (parsed.empty()) return refuse("set-output: \"name\" must not be empty");
        name = parsed;
      }

      if (sourceId) o->sourceId = *sourceId;
      if (target) o->target = *target;
      if (codec) o->codec = *codec;
      if (quality) o->quality = *quality;
      if (alpha) o->alpha = *alpha;
      if (name) o->name = *name;
      return json{{"ok", true}}.dump();
    }

    if (cmd == "remove-output") {
      if (!j.contains("id") || !j["id"].is_string()) return refuse("remove-output needs a text \"id\"");
      const std::string id = j["id"];
      if (bridge_.findOutput(id) == nullptr) return refuse("remove-output: no such output: " + id);
      bridge_.outputs.erase(
          std::remove_if(bridge_.outputs.begin(), bridge_.outputs.end(),
                         [&](const Output& o) { return o.id == id; }),
          bridge_.outputs.end());
      return json{{"ok", true}}.dump();
    }

    if (cmd == "set-source") {
      if (!j.contains("id") || !j["id"].is_string()) return refuse("set-source needs a text \"id\"");
      const std::string id = j["id"];
      Source* s = bridge_.findSource(id);
      if (s == nullptr) return refuse("set-source: no such source: " + id);

      std::optional<SourceKind> kind;
      if (j.contains("kind")) {
        if (!j["kind"].is_string()) return refuse("set-source: \"kind\" must be text");
        const auto parsed = sourceKindFromString(j["kind"].get<std::string>());
        if (!parsed) return refuse("set-source: unknown \"kind\": " + j["kind"].get<std::string>());
        kind = *parsed;
      }
      std::optional<std::string> address;
      if (j.contains("address")) {
        if (!j["address"].is_string()) return refuse("set-source: \"address\" must be text");
        const std::string parsed = j["address"];
        if (parsed.empty()) return refuse("set-source: \"address\" must not be empty");
        address = parsed;
      }
      // codec is optional by nature: absent leaves it, null clears it (an OMT or NDI source).
      const bool codecPresent = j.contains("codec");
      std::optional<HxCodec> codec;
      if (codecPresent && !j["codec"].is_null()) {
        if (!j["codec"].is_string()) return refuse("set-source: \"codec\" must be text or null");
        const auto parsed = hxCodecFromString(j["codec"].get<std::string>());
        if (!parsed) return refuse("set-source: unknown \"codec\": " + j["codec"].get<std::string>());
        codec = *parsed;
      }

      if (kind) s->kind = *kind;
      if (address) s->address = *address;
      if (codecPresent) s->codec = codec;
      return json{{"ok", true}}.dump();
    }

    if (cmd == "set-settings") {
      std::optional<OutputTarget> target;
      if (j.contains("defaultTarget")) {
        if (!j["defaultTarget"].is_string()) return refuse("set-settings: \"defaultTarget\" must be text");
        const auto parsed = outputTargetFromString(j["defaultTarget"].get<std::string>());
        if (!parsed)
          return refuse("set-settings: unknown \"defaultTarget\": " +
                        j["defaultTarget"].get<std::string>());
        target = *parsed;
      }
      std::optional<Quality> quality;
      if (j.contains("defaultQuality")) {
        if (!j["defaultQuality"].is_string())
          return refuse("set-settings: \"defaultQuality\" must be text");
        const auto parsed = qualityFromString(j["defaultQuality"].get<std::string>());
        if (!parsed)
          return refuse("set-settings: unknown \"defaultQuality\": " +
                        j["defaultQuality"].get<std::string>());
        quality = *parsed;
      }
      std::optional<int> interval;
      if (j.contains("thumbnailIntervalMs")) {
        if (!j["thumbnailIntervalMs"].is_number_integer())
          return refuse("set-settings: \"thumbnailIntervalMs\" must be an integer");
        const long long parsed = j["thumbnailIntervalMs"].get<long long>();
        if (parsed < 0 || parsed > 60000)
          return refuse("set-settings: \"thumbnailIntervalMs\" must be 0..60000");
        interval = static_cast<int>(parsed);
      }
      std::optional<bool> startWithApp;
      if (j.contains("startEngineWithApp")) {
        if (!j["startEngineWithApp"].is_boolean())
          return refuse("set-settings: \"startEngineWithApp\" must be true or false");
        startWithApp = j["startEngineWithApp"].get<bool>();
      }

      if (target) bridge_.settings.defaultTarget = *target;
      if (quality) bridge_.settings.defaultQuality = *quality;
      if (interval) bridge_.settings.thumbnailIntervalMs = *interval;
      if (startWithApp) bridge_.settings.startEngineWithApp = *startWithApp;
      return json{{"ok", true}}.dump();
    }

    if (cmd == "discover") {
      const Discovery found = discovery_ ? discovery_() : Discovery{};
      return json{{"ok", true}, {"omt", found.omt}, {"ndi", found.ndi}}.dump();
    }

    return refuse("unknown command: " + cmd);
  } catch (const json::exception&) {
    return refuse("malformed command");
  }
}

}  // namespace ob
