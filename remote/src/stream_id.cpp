#include "stream_id.h"

#include <stdexcept>

namespace bmb {
namespace {

constexpr std::string_view kPrefix = "#!::";
constexpr size_t kMaxIdBytes = 512;
constexpr size_t kMaxPartChars = 64;

bool validPart(std::string_view p) {
  if (p.empty() || p.size() > kMaxPartChars) return false;
  for (const char ch : p) {
    const auto c = static_cast<unsigned char>(ch);
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

bool validToken(std::string_view t) {
  for (const char ch : t) {
    const auto c = static_cast<unsigned char>(ch);
    if (c < 0x20 || c == 0x7f || ch == ',') return false;
  }
  return true;
}

const char* modeName(StreamMode mode) {
  switch (mode) {
    case StreamMode::Publish: return "publish";
    case StreamMode::Request: return "request";
    case StreamMode::Bidirectional: return "bidirectional";
  }
  return "";
}

std::optional<StreamMode> parseMode(std::string_view s) {
  if (s == "publish") return StreamMode::Publish;
  if (s == "request") return StreamMode::Request;
  if (s == "bidirectional") return StreamMode::Bidirectional;
  return std::nullopt;
}

}  // namespace

std::string formatStreamId(const StreamId& id) {
  if (!validPart(id.session) || !validPart(id.bridge) || !validPart(id.stream) || !validToken(id.token))
    throw std::invalid_argument("invalid stream id part");
  std::string s = std::string(kPrefix) + "r=" + resourceOf(id) + ",m=" + modeName(id.mode);
  if (!id.token.empty()) s += ",u=" + id.token;
  if (s.size() > kMaxIdBytes) throw std::invalid_argument("stream id longer than 512 bytes");
  return s;
}

std::optional<StreamId> parseStreamId(std::string_view s) {
  if (s.size() > kMaxIdBytes || s.substr(0, kPrefix.size()) != kPrefix) return std::nullopt;

  std::optional<std::string> resource, mode, token;
  std::string_view rest = s.substr(kPrefix.size());
  while (!rest.empty()) {
    const auto comma = rest.find(',');
    const std::string_view item = rest.substr(0, comma);
    rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
    const auto eq = item.find('=');
    if (eq == std::string_view::npos) return std::nullopt;
    const auto key = item.substr(0, eq);
    const auto value = item.substr(eq + 1);
    if (key == "r") resource = std::string(value);
    else if (key == "m") mode = std::string(value);
    else if (key == "u") token = std::string(value);
  }
  if (!resource || !mode) return std::nullopt;
  const auto parsedMode = parseMode(*mode);
  if (!parsedMode) return std::nullopt;

  std::string_view r = *resource;
  if (r.substr(0, 4) != "bmb/") return std::nullopt;
  r.remove_prefix(4);
  const auto slash1 = r.find('/');
  if (slash1 == std::string_view::npos) return std::nullopt;
  const auto slash2 = r.find('/', slash1 + 1);
  if (slash2 == std::string_view::npos) return std::nullopt;
  if (r.find('/', slash2 + 1) != std::string_view::npos) return std::nullopt;
  const auto session = r.substr(0, slash1);
  const auto bridge = r.substr(slash1 + 1, slash2 - slash1 - 1);
  const auto stream = r.substr(slash2 + 1);
  if (!validPart(session) || !validPart(bridge) || !validPart(stream)) return std::nullopt;

  return StreamId{std::string(session), std::string(bridge), std::string(stream), *parsedMode,
                  token.value_or("")};
}

std::string resourceOf(const StreamId& id) {
  return "bmb/" + id.session + "/" + id.bridge + "/" + id.stream;
}

}  // namespace bmb