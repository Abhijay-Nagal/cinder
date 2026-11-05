// The key-value data itself: a hash map from key to value + expiry time.
//
// Expiry is "lazy": an expired key is only removed when someone touches it.
// That keeps every operation O(1) on average, but a key that expires and is
// never read again stays in memory. (V2 adds active expiry to fix that.)
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace cinder {

// Milliseconds on a monotonic clock. Monotonic so that changing the system
// time (e.g. NTP sync) can't make keys expire early or live forever.
std::int64_t now_ms();

class Store {
public:
    static constexpr std::int64_t kNoExpiry = -1;

    // Returns the value, or nothing if the key is missing or expired.
    std::optional<std::string> get(const std::string& key);

    // Sets the value. `expire_at_ms` is an absolute time or kNoExpiry.
    void set(const std::string& key, std::string value,
             std::int64_t expire_at_ms = kNoExpiry);

    bool exists(const std::string& key);
    bool del(const std::string& key);  // true if a key was removed

    // Sets an absolute expiry time. False if the key doesn't exist.
    bool expire_at(const std::string& key, std::int64_t expire_at_ms);

    // Remaining time to live in ms: -2 = no such key, -1 = no expiry.
    std::int64_t ttl_ms(const std::string& key);

    std::size_t size() const { return map_.size(); }
    void clear() { map_.clear(); }

private:
    struct Entry {
        std::string value;
        std::int64_t expire_at_ms = kNoExpiry;
    };

    // Finds a live entry, deleting it first if it has expired.
    Entry* find_live(const std::string& key);

    std::unordered_map<std::string, Entry> map_;
};

}  // namespace cinder
