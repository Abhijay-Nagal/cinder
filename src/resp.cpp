#include "resp.h"

#include <charconv>

namespace resp {
namespace {

// Limits protect the server from a client that claims a huge size.
constexpr std::int64_t kMaxArgs = 1024 * 1024;
constexpr std::int64_t kMaxBulkLen = 512LL * 1024 * 1024;  // same as Redis
constexpr std::size_t kMaxInlineLen = 64 * 1024;

// Reads an integer terminated by \r\n starting at `pos`.
// Returns false with `need_more` set if the line is not complete yet.
bool read_int_line(std::string_view in, std::size_t& pos, std::int64_t& value,
                   bool& need_more, std::string& err) {
    std::size_t end = in.find("\r\n", pos);
    if (end == std::string_view::npos) {
        need_more = true;
        return false;
    }
    const char* first = in.data() + pos;
    const char* last = in.data() + end;
    auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc() || ptr != last) {
        err = "Protocol error: invalid length";
        return false;
    }
    pos = end + 2;
    return true;
}

ParseResult parse_array(std::string_view in, std::vector<std::string>& args) {
    std::size_t pos = 1;  // skip '*'
    std::int64_t count = 0;
    bool need_more = false;
    std::string err;

    if (!read_int_line(in, pos, count, need_more, err)) {
        if (need_more) return {Status::Incomplete, 0, {}};
        return {Status::Error, 0, err};
    }
    if (count < 0 || count > kMaxArgs) {
        return {Status::Error, 0, "Protocol error: invalid multibulk length"};
    }

    args.clear();
    args.reserve(static_cast<std::size_t>(count));
    for (std::int64_t i = 0; i < count; ++i) {
        if (pos >= in.size()) return {Status::Incomplete, 0, {}};
        if (in[pos] != '$') {
            return {Status::Error, 0, "Protocol error: expected '$'"};
        }
        ++pos;
        std::int64_t len = 0;
        if (!read_int_line(in, pos, len, need_more, err)) {
            if (need_more) return {Status::Incomplete, 0, {}};
            return {Status::Error, 0, err};
        }
        if (len < 0 || len > kMaxBulkLen) {
            return {Status::Error, 0, "Protocol error: invalid bulk length"};
        }
        // The payload plus its trailing \r\n must be fully buffered.
        std::size_t ulen = static_cast<std::size_t>(len);
        if (in.size() - pos < ulen + 2) return {Status::Incomplete, 0, {}};
        if (in[pos + ulen] != '\r' || in[pos + ulen + 1] != '\n') {
            return {Status::Error, 0, "Protocol error: bad bulk terminator"};
        }
        args.emplace_back(in.substr(pos, ulen));
        pos += ulen + 2;
    }
    return {Status::Complete, pos, {}};
}

ParseResult parse_inline(std::string_view in, std::vector<std::string>& args) {
    std::size_t end = in.find('\n');
    if (end == std::string_view::npos) {
        if (in.size() > kMaxInlineLen) {
            return {Status::Error, 0, "Protocol error: inline request too long"};
        }
        return {Status::Incomplete, 0, {}};
    }
    std::string_view line = in.substr(0, end);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

    args.clear();
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && line[i] == ' ') ++i;
        std::size_t start = i;
        while (i < line.size() && line[i] != ' ') ++i;
        if (i > start) args.emplace_back(line.substr(start, i - start));
    }
    return {Status::Complete, end + 1, {}};
}

}  // namespace

ParseResult parse_command(std::string_view input, std::vector<std::string>& args) {
    if (input.empty()) return {Status::Incomplete, 0, {}};
    if (input[0] == '*') return parse_array(input, args);
    return parse_inline(input, args);
}

void simple_string(std::string& out, std::string_view s) {
    out += '+';
    out += s;
    out += "\r\n";
}

void error(std::string& out, std::string_view msg) {
    out += '-';
    out += msg;
    out += "\r\n";
}

void integer(std::string& out, std::int64_t n) {
    out += ':';
    out += std::to_string(n);
    out += "\r\n";
}

void bulk_string(std::string& out, std::string_view s) {
    out += '$';
    out += std::to_string(s.size());
    out += "\r\n";
    out += s;
    out += "\r\n";
}

void null_bulk(std::string& out) { out += "$-1\r\n"; }

void array_header(std::string& out, std::size_t n) {
    out += '*';
    out += std::to_string(n);
    out += "\r\n";
}

}  // namespace resp
