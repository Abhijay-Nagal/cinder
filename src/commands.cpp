#include "commands.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <functional>
#include <string_view>
#include <unordered_map>

#include "resp.h"

namespace cinder {
namespace {

using Args = std::vector<std::string>;

std::string upper(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return r;
}

bool parse_int(const std::string& s, std::int64_t& out) {
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && ptr == s.data() + s.size();
}

void wrong_args(std::string& out, const std::string& name) {
    resp::error(out, "ERR wrong number of arguments for '" + name + "' command");
}

void not_integer(std::string& out) {
    resp::error(out, "ERR value is not an integer or out of range");
}

// ---- command implementations -------------------------------------------

void cmd_ping(Store&, const Args& a, std::string& out) {
    if (a.size() == 1) resp::simple_string(out, "PONG");
    else if (a.size() == 2) resp::bulk_string(out, a[1]);
    else wrong_args(out, "ping");
}

void cmd_echo(Store&, const Args& a, std::string& out) {
    if (a.size() != 2) return wrong_args(out, "echo");
    resp::bulk_string(out, a[1]);
}

void cmd_get(Store& s, const Args& a, std::string& out) {
    if (a.size() != 2) return wrong_args(out, "get");
    auto v = s.get(a[1]);
    if (v) resp::bulk_string(out, *v);
    else resp::null_bulk(out);
}

// SET key value [EX seconds | PX milliseconds] [NX | XX]
void cmd_set(Store& s, const Args& a, std::string& out) {
    if (a.size() < 3) return wrong_args(out, "set");
    std::int64_t expire_at = Store::kNoExpiry;
    bool nx = false, xx = false;

    for (std::size_t i = 3; i < a.size(); ++i) {
        std::string opt = upper(a[i]);
        if ((opt == "EX" || opt == "PX") && i + 1 < a.size()) {
            std::int64_t n = 0;
            if (!parse_int(a[i + 1], n)) return not_integer(out);
            if (n <= 0) return resp::error(out, "ERR invalid expire time in 'set' command");
            expire_at = now_ms() + (opt == "EX" ? n * 1000 : n);
            ++i;
        } else if (opt == "NX") {
            nx = true;
        } else if (opt == "XX") {
            xx = true;
        } else {
            return resp::error(out, "ERR syntax error");
        }
    }
    if (nx && xx) return resp::error(out, "ERR syntax error");

    bool present = s.exists(a[1]);
    if ((nx && present) || (xx && !present)) return resp::null_bulk(out);
    s.set(a[1], a[2], expire_at);
    resp::simple_string(out, "OK");
}

void cmd_del(Store& s, const Args& a, std::string& out) {
    if (a.size() < 2) return wrong_args(out, "del");
    std::int64_t removed = 0;
    for (std::size_t i = 1; i < a.size(); ++i) removed += s.del(a[i]) ? 1 : 0;
    resp::integer(out, removed);
}

void cmd_exists(Store& s, const Args& a, std::string& out) {
    if (a.size() < 2) return wrong_args(out, "exists");
    std::int64_t n = 0;
    for (std::size_t i = 1; i < a.size(); ++i) n += s.exists(a[i]) ? 1 : 0;
    resp::integer(out, n);
}

// EXPIRE key seconds / PEXPIRE key milliseconds
void expire_generic(Store& s, const Args& a, std::string& out, bool millis) {
    if (a.size() != 3) return wrong_args(out, millis ? "pexpire" : "expire");
    std::int64_t n = 0;
    if (!parse_int(a[2], n)) return not_integer(out);
    if (n <= 0) {
        // A non-positive timeout deletes the key immediately (Redis behaviour).
        resp::integer(out, s.del(a[1]) ? 1 : 0);
        return;
    }
    std::int64_t at = now_ms() + (millis ? n : n * 1000);
    resp::integer(out, s.expire_at(a[1], at) ? 1 : 0);
}

void cmd_expire(Store& s, const Args& a, std::string& out) { expire_generic(s, a, out, false); }
void cmd_pexpire(Store& s, const Args& a, std::string& out) { expire_generic(s, a, out, true); }

void ttl_generic(Store& s, const Args& a, std::string& out, bool millis) {
    if (a.size() != 2) return wrong_args(out, millis ? "pttl" : "ttl");
    std::int64_t ms = s.ttl_ms(a[1]);
    if (ms < 0 || millis) return resp::integer(out, ms);
    resp::integer(out, (ms + 500) / 1000);  // round to nearest second, like Redis
}

void cmd_ttl(Store& s, const Args& a, std::string& out) { ttl_generic(s, a, out, false); }
void cmd_pttl(Store& s, const Args& a, std::string& out) { ttl_generic(s, a, out, true); }

void cmd_dbsize(Store& s, const Args& a, std::string& out) {
    if (a.size() != 1) return wrong_args(out, "dbsize");
    resp::integer(out, static_cast<std::int64_t>(s.size()));
}

void cmd_flushall(Store& s, const Args&, std::string& out) {
    s.clear();
    resp::simple_string(out, "OK");
}

// redis-cli sends COMMAND DOCS on startup and redis-benchmark sends
// CONFIG GET; an empty array is a valid "nothing to report" answer.
void cmd_empty_array(Store&, const Args&, std::string& out) { resp::array_header(out, 0); }

using Handler = std::function<void(Store&, const Args&, std::string&)>;

const std::unordered_map<std::string, Handler>& table() {
    static const std::unordered_map<std::string, Handler> t = {
        {"PING", cmd_ping},       {"ECHO", cmd_echo},       {"GET", cmd_get},
        {"SET", cmd_set},         {"DEL", cmd_del},         {"EXISTS", cmd_exists},
        {"EXPIRE", cmd_expire},   {"PEXPIRE", cmd_pexpire}, {"TTL", cmd_ttl},
        {"PTTL", cmd_pttl},       {"DBSIZE", cmd_dbsize},   {"FLUSHALL", cmd_flushall},
        {"COMMAND", cmd_empty_array}, {"CONFIG", cmd_empty_array},
    };
    return t;
}

}  // namespace

void execute(Store& store, const std::vector<std::string>& args, std::string& out) {
    if (args.empty()) return;  // blank inline line: Redis sends nothing back
    const auto& t = table();
    auto it = t.find(upper(args[0]));
    if (it == t.end()) {
        resp::error(out, "ERR unknown command '" + args[0] + "'");
        return;
    }
    it->second(store, args, out);
}

}  // namespace cinder
