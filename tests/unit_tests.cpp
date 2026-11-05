// Small dependency-free unit tests for the parser, store and commands.
// Run with: ctest --test-dir build  (or ./build/unit_tests)
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "commands.h"
#include "resp.h"
#include "store.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::cerr << __FILE__ << ":" << __LINE__ << ": FAILED: " #cond "\n"; \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

std::string run(cinder::Store& s, std::vector<std::string> args) {
    std::string out;
    cinder::execute(s, args, out);
    return out;
}

void test_parse_complete_array() {
    std::vector<std::string> args;
    std::string in = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$5\r\nhello\r\n";
    auto r = resp::parse_command(in, args);
    CHECK(r.status == resp::Status::Complete);
    CHECK(r.consumed == in.size());
    CHECK((args == std::vector<std::string>{"SET", "k", "hello"}));
}

void test_parse_incomplete_at_every_split() {
    // Whatever byte the network splits the command at, the parser must say
    // "incomplete" and never mis-parse.
    std::string in = "*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n";
    std::vector<std::string> args;
    for (std::size_t cut = 0; cut < in.size(); ++cut) {
        auto r = resp::parse_command(std::string_view(in).substr(0, cut), args);
        CHECK(r.status == resp::Status::Incomplete);
    }
}

void test_parse_pipelined() {
    std::string in = "*1\r\n$4\r\nPING\r\n*2\r\n$4\r\nECHO\r\n$2\r\nhi\r\n";
    std::vector<std::string> args;
    auto r1 = resp::parse_command(in, args);
    CHECK(r1.status == resp::Status::Complete);
    CHECK(args == std::vector<std::string>{"PING"});
    auto r2 = resp::parse_command(std::string_view(in).substr(r1.consumed), args);
    CHECK(r2.status == resp::Status::Complete);
    CHECK((args == std::vector<std::string>{"ECHO", "hi"}));
    CHECK(r1.consumed + r2.consumed == in.size());
}

void test_parse_binary_safe() {
    // Bulk strings carry their length, so values may contain \r\n.
    std::string in = "*2\r\n$4\r\nECHO\r\n$4\r\na\r\nb\r\n";
    std::vector<std::string> args;
    auto r = resp::parse_command(in, args);
    CHECK(r.status == resp::Status::Complete);
    CHECK(args.size() == 2 && args[1] == "a\r\nb");
}

void test_parse_inline_and_errors() {
    std::vector<std::string> args;
    auto r = resp::parse_command("SET  a   b\r\n", args);
    CHECK(r.status == resp::Status::Complete);
    CHECK((args == std::vector<std::string>{"SET", "a", "b"}));

    CHECK(resp::parse_command("*x\r\n", args).status == resp::Status::Error);
    CHECK(resp::parse_command("*1\r\n#3\r\n", args).status == resp::Status::Error);
    CHECK(resp::parse_command("*1\r\n$3\r\nabcXY", args).status == resp::Status::Error);
}

void test_commands() {
    cinder::Store s;
    CHECK(run(s, {"PING"}) == "+PONG\r\n");
    CHECK(run(s, {"get", "missing"}) == "$-1\r\n");
    CHECK(run(s, {"SET", "k", "v"}) == "+OK\r\n");
    CHECK(run(s, {"GET", "k"}) == "$1\r\nv\r\n");
    CHECK(run(s, {"SET", "k", "w", "NX"}) == "$-1\r\n");
    CHECK(run(s, {"SET", "other", "x", "XX"}) == "$-1\r\n");
    CHECK(run(s, {"EXISTS", "k", "k", "nope"}) == ":2\r\n");
    CHECK(run(s, {"TTL", "k"}) == ":-1\r\n");
    CHECK(run(s, {"TTL", "nope"}) == ":-2\r\n");
    CHECK(run(s, {"EXPIRE", "k", "100"}) == ":1\r\n");
    CHECK(run(s, {"TTL", "k"}) == ":100\r\n");
    CHECK(run(s, {"SET", "k", "v2"}) == "+OK\r\n");  // SET clears the TTL
    CHECK(run(s, {"TTL", "k"}) == ":-1\r\n");
    CHECK(run(s, {"DEL", "k", "nope"}) == ":1\r\n");
    CHECK(run(s, {"DBSIZE"}) == ":0\r\n");
    CHECK(run(s, {"SET", "n", "1", "EX", "abc"}).rfind("-ERR", 0) == 0);
    CHECK(run(s, {"NOPE"}).rfind("-ERR unknown command", 0) == 0);
    CHECK(run(s, {"GET"}).rfind("-ERR wrong number", 0) == 0);
}

void test_lazy_expiry() {
    cinder::Store s;
    CHECK(run(s, {"SET", "t", "v", "PX", "30"}) == "+OK\r\n");
    CHECK(run(s, {"GET", "t"}) == "$1\r\nv\r\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(s.size() == 1);                     // still in memory...
    CHECK(run(s, {"GET", "t"}) == "$-1\r\n"); // ...until it is touched
    CHECK(s.size() == 0);
}

}  // namespace

int main() {
    test_parse_complete_array();
    test_parse_incomplete_at_every_split();
    test_parse_pipelined();
    test_parse_binary_safe();
    test_parse_inline_and_errors();
    test_commands();
    test_lazy_expiry();
    if (g_failures) {
        std::cerr << g_failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all unit tests passed\n";
    return EXIT_SUCCESS;
}
