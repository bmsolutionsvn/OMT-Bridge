// OMT-Bridge-Engine.exe: headless. Task 15 replaces this body with the control server and graph.
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  int port = 0;
  std::string token;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string k = argv[i];
    if (k == "--port") port = std::atoi(argv[i + 1]);
    else if (k == "--token") token = argv[i + 1];
  }
  if (port == 0 || token.empty()) {
    std::fprintf(stderr, "usage: omt-bridge-engine --port <n> --token <s>\n");
    return 2;
  }
  std::printf("OMT Bridge engine listening on 127.0.0.1:%d\n", port);
  std::fflush(stdout);
  // Placeholder: exit cleanly so the shell's supervisor can be exercised.
  for (;;) {
    std::string line;
    if (!std::getline(std::cin, line)) break;
    if (line == "quit") break;
  }
  return 0;
}