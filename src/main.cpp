#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

#include "server.h"

namespace {
void on_signal(int) { cinder::Server::stop(); }
}  // namespace

int main(int argc, char** argv) {
    int port = 6380;  // default: one above Redis's 6379, so both can run
    if (argc > 1) port = std::atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        std::cerr << "usage: cinder [port]\n";
        return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    try {
        cinder::Server server(static_cast<std::uint16_t>(port));
        std::cout << "cinder listening on port " << port << std::endl;
        server.run();
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << '\n';
        return 1;
    }
    std::cout << "cinder stopped" << std::endl;
    return 0;
}
