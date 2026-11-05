#include "server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "commands.h"
#include "resp.h"

namespace cinder {
namespace {

// Set from a signal handler, so it must be a lock-free atomic flag.
volatile std::sig_atomic_t g_running = 1;

// A client may not buffer more than this many unparsed bytes.
constexpr std::size_t kMaxReadBuffer = 64 * 1024 * 1024;

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw std::runtime_error(std::string("fcntl: ") + std::strerror(errno));
    }
}

}  // namespace

Server::Server(std::uint16_t port) {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) throw std::runtime_error(std::string("socket: ") + std::strerror(errno));

    // Lets the server restart immediately instead of waiting for old
    // connections in TIME_WAIT to clear.
    int yes = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        throw std::runtime_error(std::string("bind: ") + std::strerror(errno));
    }
    if (listen(listen_fd_, SOMAXCONN) < 0) {
        throw std::runtime_error(std::string("listen: ") + std::strerror(errno));
    }
    set_nonblocking(listen_fd_);
}

Server::~Server() {
    for (auto& [fd, conn] : conns_) ::close(fd);
    if (listen_fd_ >= 0) ::close(listen_fd_);
}

void Server::stop() { g_running = 0; }

void Server::run() {
    std::vector<pollfd> fds;
    while (g_running) {
        // 1. Build the list of sockets to watch. Every client is watched for
        //    input; clients with pending replies are also watched for "can
        //    write" so we never block on a slow reader.
        fds.clear();
        fds.push_back({listen_fd_, POLLIN, 0});
        for (auto& [fd, conn] : conns_) {
            short events = POLLIN;
            if (!conn->wbuf.empty()) events |= POLLOUT;
            fds.push_back({fd, events, 0});
        }

        // 2. Sleep until at least one socket is ready (or a signal arrives).
        int ready = poll(fds.data(), fds.size(), -1);
        if (ready < 0) {
            if (errno == EINTR) continue;  // interrupted by a signal
            throw std::runtime_error(std::string("poll: ") + std::strerror(errno));
        }

        // 3. Handle every ready socket.
        if (fds[0].revents & POLLIN) accept_clients();
        for (std::size_t i = 1; i < fds.size(); ++i) {
            const pollfd& p = fds[i];
            if (p.revents == 0) continue;
            auto it = conns_.find(p.fd);
            if (it == conns_.end()) continue;
            Conn& c = *it->second;

            if (p.revents & (POLLERR | POLLNVAL)) {
                close_conn(p.fd);
                continue;
            }
            if (p.revents & (POLLIN | POLLHUP)) handle_read(c);
            if (conns_.count(p.fd) && (p.revents & POLLOUT)) handle_write(c);
        }
    }
}

void Server::accept_clients() {
    // The listening socket is non-blocking, so accept until the kernel's
    // queue of pending connections is empty.
    while (true) {
        int fd = accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            return;  // e.g. too many open files: try again on the next loop
        }
        set_nonblocking(fd);
        // Send small replies immediately instead of waiting to batch them.
        int yes = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        conns_[fd] = std::make_unique<Conn>(Conn{fd, {}, {}});
    }
}

void Server::handle_read(Conn& c) {
    char buf[16 * 1024];
    while (true) {
        ssize_t n = ::read(c.fd, buf, sizeof(buf));
        if (n > 0) {
            c.rbuf.append(buf, static_cast<std::size_t>(n));
            if (c.rbuf.size() > kMaxReadBuffer) {
                close_conn(c.fd);
                return;
            }
            continue;
        }
        if (n == 0) {  // the client closed its end
            close_conn(c.fd);
            return;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // drained
        close_conn(c.fd);
        return;
    }

    process_buffer(c);
    // Try to send replies right away; most of the time the socket is
    // writable and this saves a full trip around the event loop.
    if (!c.wbuf.empty()) handle_write(c);
}

void Server::process_buffer(Conn& c) {
    // Parse as many complete commands as are buffered (clients may
    // "pipeline" many commands in one packet). A half-received command
    // stays in rbuf until the rest arrives.
    std::size_t offset = 0;
    std::vector<std::string> args;
    while (offset < c.rbuf.size() && !c.closing) {
        std::string_view pending(c.rbuf.data() + offset, c.rbuf.size() - offset);
        resp::ParseResult r = resp::parse_command(pending, args);
        if (r.status == resp::Status::Incomplete) break;
        if (r.status == resp::Status::Error) {
            resp::error(c.wbuf, "ERR " + r.error);
            c.closing = true;
            break;
        }
        offset += r.consumed;
        execute(store_, args, c.wbuf);
    }
    // Drop the consumed bytes once, instead of after every command.
    c.rbuf.erase(0, offset);
}

void Server::handle_write(Conn& c) {
    std::size_t sent = 0;
    while (sent < c.wbuf.size()) {
        // MSG_NOSIGNAL: a client that disconnected must not kill the
        // server with SIGPIPE; we get EPIPE instead.
        ssize_t n = ::send(c.fd, c.wbuf.data() + sent, c.wbuf.size() - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;  // kernel buffer full
        close_conn(c.fd);
        return;
    }
    c.wbuf.erase(0, sent);
    if (c.wbuf.empty() && c.closing) close_conn(c.fd);
}

void Server::close_conn(int fd) {
    ::close(fd);
    conns_.erase(fd);
}

}  // namespace cinder
