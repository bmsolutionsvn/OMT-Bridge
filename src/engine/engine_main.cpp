// OMT-Bridge-Engine.exe: headless. Owns the sources, outputs and bridge graph; keeps them
// running when the UI is closed (design 6.1).
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include <ixwebsocket/IXNetSystem.h>

#include "core/bridge_store.h"
#include "engine/engine.h"

namespace {

HANDLE g_stopEvent = nullptr;

BOOL WINAPI onConsoleControl(DWORD) {
  if (g_stopEvent) SetEvent(g_stopEvent);
  return TRUE;  // handled, so the default handler does not kill us mid-frame
}

}  // namespace

int main(int argc, char** argv) {
  // Winsock must be initialised before ixwebsocket creates a socket, or listen() fails with
  // WSA 10093 (WSANOTINITIALISED). BreadCamera's engine_main does exactly this; ixwebsocket does
  // not initialise itself.
  ix::initNetSystem();

  int port = 0;
  std::string token, bridgeFile;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string k = argv[i];
    if (k == "--port") port = std::atoi(argv[i + 1]);
    else if (k == "--token") token = argv[i + 1];
    else if (k == "--bridge") bridgeFile = argv[i + 1];
  }
  if (port == 0) {
    std::fprintf(stderr, "usage: omt-bridge-engine --port <n> [--token <s>] [--bridge <file>]\n");
    return 2;
  }
  // The token is the only guard on the loopback control channel, so it must come from a CSPRNG.
  // Reuse the lifted randomToken() (BCryptGenRandom); std::rand() is unseeded and predictable.
  if (token.empty()) token = ob::randomToken();

  ob::Engine engine(port, token, bridgeFile);

  if (!bridgeFile.empty()) {
    if (auto b = ob::loadBridge(bridgeFile)) {
      engine.apply(*b);
    } else {
      std::fprintf(stderr, "warning: could not read %s; starting with an empty bridge\n",
                   bridgeFile.c_str());
    }
  }

  engine.start();

  // Report readiness on stdout; the UI reads this first line.
  std::printf("READY %s\n", token.c_str());
  std::fflush(stdout);

  // Run until the process is asked to stop. Deliberately NOT tied to stdin: the UI launches the
  // engine without a console and may close at any moment, and design 6.1 says closing the window
  // leaves the engine bridging. Ctrl+C (or a console close) ends it cleanly.
  g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (g_stopEvent != nullptr) {
    SetConsoleCtrlHandler(onConsoleControl, TRUE);
    WaitForSingleObject(g_stopEvent, INFINITE);
    SetConsoleCtrlHandler(onConsoleControl, FALSE);
    CloseHandle(g_stopEvent);
    g_stopEvent = nullptr;
  } else {
    Sleep(INFINITE);  // could not arm the handler: keep bridging until terminated
  }

  engine.stop();
  return 0;
}
