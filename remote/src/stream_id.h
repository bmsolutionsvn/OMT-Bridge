#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace bmb {

enum class StreamMode { Publish, Request, Bidirectional };

struct StreamId {
  std::string session, bridge, stream;
  StreamMode mode;
  std::string token;
};

// "#!::r=bmb/<session>/<bridge>/<stream>,m=<mode>[,u=<token>]"
std::string formatStreamId(const StreamId&);
std::optional<StreamId> parseStreamId(std::string_view);
std::string resourceOf(const StreamId&);

}  // namespace bmb