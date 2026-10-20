// OMT-Bridge-Engine.exe: headless. Owns the sources, outputs and bridge graph; keeps them
// running when the UI is closed (design 6.1).
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>

#include <ixwebsocket/IXNetSystem.h>
#include <nlohmann/json.hpp>

#include "core/bridge_store.h"
#include "engine/engine.h"

namespace {

HANDLE g_stopEvent = nullptr;

BOOL WINAPI onConsoleControl(DWORD) {
  if (g_stopEvent) SetEvent(g_stopEvent);
  return TRUE;  // handled, so the default handler does not kill us mid-frame
}

// The UI reads this to adopt a running engine after it was closed and reopened. Written to a
// sibling temp file and moved into place, so a reader never sees half a file. A crashed engine
// leaves it behind; the UI checks the pid is still an omt-bridge-engine process.
void writeDiscovery(const std::filesystem::path& path, int port, const std::string& token) {
  const nlohmann::json j{{"pid", GetCurrentProcessId()}, {"port", port}, {"token", token}};
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
  out << j.dump();
  out.flush();
  if (!out) {
    out.close();
    std::error_code ignored;
    std::filesystem::remove(tmp, ignored);
    return;
  }
  out.close();
  MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

}  // namespace

int main(int argc, char** argv) {
  // Winsock must be initialised before ixwebsocket creates a socket, or listen() fails with
  // WSA 10093 (WSANOTINITIALISED). BreadCamera's engine_main does exactly this; ixwebsocket does
  // not initialise itself.
  if (!ix::initNetSystem()) {
    std::fprintf(stderr, "Winsock initialisation failed\n");
    return 1;
  }

  int port = 0;
  std::string token, bridgeFile, discoveryFile;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string k = argv[i];
    if (k == "--port") port = std::atoi(argv[i + 1]);
    else if (k == "--token") token = argv[i + 1];
    else if (k == "--bridge") bridgeFile = argv[i + 1];
    else if (k == "--discovery") discoveryFile = argv[i + 1];
  }
  // A port outside 1..65535 is a usage error, not "run without a control channel": atoi("-1") and
  // atoi("70000") are non-zero, so a `== 0` check would let them through and the engine would
  // bridge with no way to command or stop it.
  if (port < 1 || port > 65535) {
    std::fprintf(stderr,
                 "usage: omt-bridge-engine --port <n> [--token <s>] [--bridge <file>] "
                 "[--discovery <file>]\n");
    return 2;
  }
  // The token is the only guard on the loopback control channel, so it must come from a CSPRNG.
  // Reuse the lifted randomToken() (BCryptGenRandom); std::rand() is unseeded and predictable.
  if (token.empty()) token = ob::randomToken();

  ob::Engine engine(port, token, bridgeFile);

  if (!bridgeFile.empty()) {
    if (auto b = ob::loadBridge(bridgeFile)) {
      try {
        engine.apply(*b);
      } catch (const std::exception& e) {
        // Applying a saved bridge can fail (thread creation, allocation). Report it like a start
        // failure: an uncaught exception would terminate with no message.
        std::fprintf(stderr, "engine apply failed: %s\n", e.what());
        return 1;
      }
    } else {
      std::fprintf(stderr, "warning: could not read %s; starting with an empty bridge\n",
                   bridgeFile.c_str());
    }
  }

  try {
    engine.start();
  } catch (const std::exception& e) {
    // A bind/listen failure surfaces here (ControlServer::start throws). Report it: an uncaught
    // exception would terminate with no message, which a supervisor sees as an unexplained loop.
    std::fprintf(stderr, "engine start failed: %s\n", e.what());
    return 1;
  }

  // Report readiness on stdout; the UI reads this first line.
  std::printf("READY %s\n", token.c_str());
  std::fflush(stdout);

  if (!discoveryFile.empty()) writeDiscovery(discoveryFile, port, token);

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
  if (!discoveryFile.empty()) {
    std::error_code ignored;
    std::filesystem::remove(discoveryFile, ignored);
  }
  return 0;
}
