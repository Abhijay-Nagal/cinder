// RESP (REdis Serialization Protocol) parsing and reply encoding.
//
// A client sends each command as an array of bulk strings, e.g. SET k v:
//   *3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n
// TCP is a byte stream, so a read() may return half a command or several
// commands at once. The parser therefore reports how many bytes it used,
// or that it needs more data.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace resp {

enum class Status {
    Complete,    // one full command parsed; `consumed` bytes were used
    Incomplete,  // need more bytes before a command can be parsed
    Error,       // malformed input; the connection should be closed
};

struct ParseResult {
    Status status;
    std::size_t consumed = 0;  // valid only when status == Complete
    std::string error;         // valid only when status == Error
};

// Parses one command from the start of `input` into `args`.
// Handles RESP arrays (what redis-cli and client libraries send) and
// "inline" commands (plain text like "PING\r\n", handy with telnet/nc).
ParseResult parse_command(std::string_view input, std::vector<std::string>& args);

// Reply encoders. Each appends one RESP value to `out`.
void simple_string(std::string& out, std::string_view s);  // +OK
void error(std::string& out, std::string_view msg);        // -ERR ...
void integer(std::string& out, std::int64_t n);            // :42
void bulk_string(std::string& out, std::string_view s);    // $3\r\nfoo
void null_bulk(std::string& out);                          // $-1 (nil)
void array_header(std::string& out, std::size_t n);        // *n

}  // namespace resp
