#include <doctest/doctest.h>

#include <stdexcept>
#include <string>

#include "stream_id.h"

using namespace bmb;

TEST_CASE("StreamId: round trip and resource") {
  const StreamId id{"show-1", "Venue", "cam1", StreamMode::Publish, "tok"};
  const auto s = formatStreamId(id);
  CHECK(s == "#!::r=bmb/show-1/Venue/cam1,m=publish,u=tok");
  const auto back = parseStreamId(s);
  REQUIRE(back);
  CHECK(back->bridge == "Venue");
  CHECK(back->mode == StreamMode::Publish);
  CHECK(resourceOf(*back) == "bmb/show-1/Venue/cam1");
  CHECK(parseStreamId("#!::m=request,r=bmb/s/b/control")->mode == StreamMode::Request);
}

TEST_CASE("StreamId: malformed ids are refused") {
  for (const char* bad : {"", "bmb/s/b/c", "#!::r=bmb/s/b,m=publish", "#!::r=bmb/s/b/c,m=send",
                          "#!::r=bmb/s/b c/x,m=publish", "#!::r=bmb/s//c,m=publish", "#!::r=other/s/b/c,m=publish"})
    CHECK_FALSE(parseStreamId(bad));
  CHECK_FALSE(parseStreamId("#!::r=bmb/s/b/" + std::string(65, 'x') + ",m=publish"));
  CHECK_THROWS_AS(formatStreamId({"s", "b/c", "x", StreamMode::Publish, ""}), std::invalid_argument);
}