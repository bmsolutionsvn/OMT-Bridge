#pragma once

#include <srt/srt.h>

#include <cstdint>
#include <string>

namespace bmb {

struct SrtOptions {
  int latencyMs = 120;
  int sndBufBytes = 0;      // 0 = libsrt default
  int rcvBufBytes = 0;      // 0 = libsrt default
  int fcPackets = 0;        // 0 = libsrt default
  int payloadSize = 1316;
  int overheadPct = 25;
  std::string passphrase;  // empty = no encryption
};

void applyOptions(SRTSOCKET, const SrtOptions&);

struct SrtStats {
  double mbpsSent, mbpsRecv;
  int64_t pktSent, pktRetrans, pktLossRecv, pktDropRecv, pktDropSend;
  double rttMs, sndBufMs, rcvBufMs, estBwMbps;
};

SrtStats statsOf(SRTSOCKET, bool clearIntervals);

class SrtLibrary {
 public:
  SrtLibrary();
  ~SrtLibrary();
  SrtLibrary(const SrtLibrary&) = delete;
  SrtLibrary& operator=(const SrtLibrary&) = delete;
};

}  // namespace bmb