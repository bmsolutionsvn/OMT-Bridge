#include "engine/command_router.h"

#include <algorithm>
#include <exception>
#include <optional>
#include <string>

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
      for (const Source& s : bridge_.sources)
        sources.push_back({{"id", s.id}, {"kind", std::string(toString(s.kind))}, {"address", s.address}});
      json outputs = json::array();
      for (const Output& o : bridge_.outputs)
        outputs.push_back({{"id", o.id},
                           {"sourceId", o.sourceId},
                           {"target", std::string(toString(o.target))},
                           {"name", o.name},
                           {"advertised", bridge_.advertisedName(o)}});
      return json{{"ok", true}, {"name", bridge_.name}, {"sources", sources}, {"outputs", outputs}}.dump();
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

    return refuse("unknown command: " + cmd);
  } catch (const json::exception&) {
    return refuse("malformed command");
  }
}

}  // namespace ob
