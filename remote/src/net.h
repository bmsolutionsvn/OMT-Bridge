#pragma once

#include <srt/srt.h>

#include <string>

#ifndef _WIN32
#include <arpa/inet.h>
#endif

namespace bmb {

inline int parseIpv4(const char* host, in_addr* out) {
#ifdef _WIN32
  return InetPtonA(AF_INET, host, out);  // ws2tcpip.h comes with srt.h on Windows
#else
  return inet_pton(AF_INET, host, out);
#endif
}

// IPv4 literal only: the bench and the relay are pointed at an address, not a name. An
// unparsable host leaves 0.0.0.0, so the connect fails with a clear SRT error.
inline sockaddr_in makeAddr(const std::string& host, int port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<uint16_t>(port));
  parseIpv4(host.c_str(), &a.sin_addr);
  return a;
}

inline int portOf(const sockaddr_in& a) { return ntohs(a.sin_port); }

}  // namespace bmb