// A single-threaded TCP server built on a poll() event loop.
//
// Instead of one thread per client, one thread asks the kernel "which of
// these sockets are ready?" (poll), then does only non-blocking work on the
// ready ones. Because only one thread touches the Store, no locks are needed.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "store.h"

namespace cinder {

class Server {
public:
    explicit Server(std::uint16_t port);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Runs the event loop until stop() is called (e.g. from a signal handler).
    void run();
    static void stop();

private:
    // Per-client state. Bytes read but not yet parsed wait in `rbuf`;
    // replies not yet sent wait in `wbuf`.
    struct Conn {
        int fd;
        std::string rbuf;
        std::string wbuf;
        bool closing = false;  // protocol error: flush the error, then close
    };

    void accept_clients();
    void handle_read(Conn& c);
    void handle_write(Conn& c);
    void process_buffer(Conn& c);
    void close_conn(int fd);

    int listen_fd_ = -1;
    Store store_;
    std::unordered_map<int, std::unique_ptr<Conn>> conns_;
};

}  // namespace cinder
