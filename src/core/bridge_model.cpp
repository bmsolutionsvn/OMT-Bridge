#include "core/bridge_model.h"

#include <algorithm>
#include <utility>

namespace ob {

std::string_view toString(SourceKind k) {
  switch (k) {
    case SourceKind::Omt: return "OMT";
    case SourceKind::OmtHx: return "OMT-HX";
    case SourceKind::Ndi: return "NDI";
  }
  return "OMT";
}

std::string_view toString(OutputTarget t) {
  switch (t) {
    case OutputTarget::Omt: return "OMT";
    case OutputTarget::OmtHx: return "OMT-HX";
    case OutputTarget::Ndi: return "NDI";
  }
  return "OMT";
}

std::string_view toString(HxCodec c) { return c == HxCodec::Hevc ? "HEVC" : "H.264"; }
std::string_view toString(Quality q) {
  switch (q) {
    case Quality::Low: return "Low";
    case Quality::Medium: return "Medium";
    case Quality::High: return "High";
  }
  return "Medium";
}
std::string_view toString(AlphaMode a) { return a == AlphaMode::Auto ? "Auto" : "Off"; }

const Source* Bridge::findSource(const std::string& id) const {
  const auto it = std::find_if(sources.begin(), sources.end(), [&](const Source& s) { return s.id == id; });
  return it == sources.end() ? nullptr : &*it;
}
Source* Bridge::findSource(const std::string& id) {
  // The const overload does the lookup; this one only removes constness from the result.
  return const_cast<Source*>(std::as_const(*this).findSource(id));
}
const Output* Bridge::findOutput(const std::string& id) const {
  const auto it = std::find_if(outputs.begin(), outputs.end(), [&](const Output& o) { return o.id == id; });
  return it == outputs.end() ? nullptr : &*it;
}
Output* Bridge::findOutput(const std::string& id) {
  return const_cast<Output*>(std::as_const(*this).findOutput(id));
}

size_t Bridge::outputsFor(const std::string& sourceId) const {
  return static_cast<size_t>(std::count_if(outputs.begin(), outputs.end(),
                                           [&](const Output& o) { return o.sourceId == sourceId; }));
}

std::string Bridge::advertisedName(const Output& o) const {
  if (name.empty()) return o.name;
  return name + " (" + o.name + ")";
}

}  // namespace ob
