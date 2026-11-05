#include "srt_util.h"

#include <stdexcept>
#include <string>

namespace bmb {
namespace {

void setInt(SRTSOCKET s, SRT_SOCKOPT opt, int value) {
  if (srt_setsockflag(s, opt, &value, sizeof value) == SRT_ERROR)
    throw std::runtime_error("SRT option " + std::to_string(opt) + ": " + srt_getlasterror_str());
}

}  // namespace

SrtLibrary::SrtLibrary() {
  if (srt_startup() == SRT_ERROR)
    throw std::runtime_error(std::string("srt_startup: ") + srt_getlasterror_str());
}

SrtLibrary::~SrtLibrary() { srt_cleanup(); }

void applyOptions(SRTSOCKET s, const SrtOptions& o) {
  setInt(s, SRTO_TRANSTYPE, SRTT_LIVE);
  setInt(s, SRTO_LATENCY, o.latencyMs);
  setInt(s, SRTO_PAYLOADSIZE, o.payloadSize);
  setInt(s, SRTO_OHEADBW, o.overheadPct);
  if (o.sndBufBytes > 0) setInt(s, SRTO_SNDBUF, o.sndBufBytes);
  if (o.rcvBufBytes > 0) setInt(s, SRTO_RCVBUF, o.rcvBufBytes);
  if (o.fcPackets > 0) setInt(s, SRTO_FC, o.fcPackets);
  if (!o.passphrase.empty()) {
    if (o.passphrase.size() < 10 || o.passphrase.size() > 79)
      throw std::runtime_error("SRT passphrase must be 10-79 characters");
    if (srt_setsockflag(s, SRTO_PASSPHRASE, o.passphrase.data(),
                        static_cast<int>(o.passphrase.size())) == SRT_ERROR)
      throw std::runtime_error(std::string("SRT passphrase: ") + srt_getlasterror_str());
    setInt(s, SRTO_PBKEYLEN, 32);
  }
}

SrtStats statsOf(SRTSOCKET s, bool clearIntervals) {
  SRT_TRACEBSTATS p{};
  if (srt_bstats(s, &p, clearIntervals ? 1 : 0) == SRT_ERROR)
    throw std::runtime_error(std::string("srt_bstats: ") + srt_getlasterror_str());
  SrtStats out{};
  out.mbpsSent = p.mbpsSendRate;
  out.mbpsRecv = p.mbpsRecvRate;
  out.pktSent = p.pktSent;
  out.pktRetrans = p.pktRetrans;
  out.pktLossRecv = p.pktRcvLoss;
  out.pktDropRecv = p.pktRcvDrop;
  out.pktDropSend = p.pktSndDrop;
  out.rttMs = p.msRTT;
  out.sndBufMs = p.msSndBuf;
  out.rcvBufMs = p.msRcvBuf;
  out.estBwMbps = p.mbpsBandwidth;
  return out;
}

}  // namespace bmb