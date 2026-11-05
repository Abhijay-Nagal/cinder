#include "store.h"

#include <chrono>

namespace cinder {

std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

Store::Entry* Store::find_live(const std::string& key) {
    auto it = map_.find(key);
    if (it == map_.end()) return nullptr;
    const std::int64_t exp = it->second.expire_at_ms;
    if (exp != kNoExpiry && exp <= now_ms()) {
        map_.erase(it);  // lazy expiry: delete on access
        return nullptr;
    }
    return &it->second;
}

std::optional<std::string> Store::get(const std::string& key) {
    Entry* e = find_live(key);
    if (!e) return std::nullopt;
    return e->value;
}

void Store::set(const std::string& key, std::string value, std::int64_t expire_at_ms) {
    Entry& e = map_[key];
    e.value = std::move(value);
    e.expire_at_ms = expire_at_ms;  // a plain SET clears any old TTL
}

bool Store::exists(const std::string& key) { return find_live(key) != nullptr; }

bool Store::del(const std::string& key) {
    if (!find_live(key)) return false;
    map_.erase(key);
    return true;
}

bool Store::expire_at(const std::string& key, std::int64_t expire_at_ms) {
    Entry* e = find_live(key);
    if (!e) return false;
    e->expire_at_ms = expire_at_ms;
    return true;
}

std::int64_t Store::ttl_ms(const std::string& key) {
    Entry* e = find_live(key);
    if (!e) return -2;
    if (e->expire_at_ms == kNoExpiry) return -1;
    return e->expire_at_ms - now_ms();
}

}  // namespace cinder
