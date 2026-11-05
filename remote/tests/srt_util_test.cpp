#include <doctest/doctest.h>

#include "srt_util.h"

using namespace bmb;

TEST_CASE("SrtOptions: applied values read back") {
  SrtLibrary lib;
  const SRTSOCKET s = srt_create_socket();
  applyOptions(s, {.latencyMs = 250, .rcvBufBytes = 64 * 1024 * 1024, .fcPackets = 60000, .passphrase = "0123456789abcdef"});
  int v = 0; int len = sizeof v;
  srt_getsockflag(s, SRTO_LATENCY, &v, &len);  CHECK(v == 250);
  srt_getsockflag(s, SRTO_FC, &v, &len);       CHECK(v == 60000);
  srt_getsockflag(s, SRTO_PBKEYLEN, &v, &len); CHECK(v == 32);
  CHECK_THROWS(applyOptions(s, {.passphrase = "short"}));
  srt_close(s);
}