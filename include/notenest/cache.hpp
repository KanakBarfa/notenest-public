#ifndef CACHE_HPP
#define CACHE_HPP

#include <optional>
#include <string>

// Interface for cache backend.
class Cache {
public:
    virtual ~Cache() = default;

    // Retrieve cached value by key.
    virtual std::optional<std::string> get(const std::string& key) = 0;

    // Set cached value with TTL in seconds.
    virtual void set(const std::string& key, const std::string& value, int ttl_seconds) = 0;

    // Delete cached value by key.
    virtual void del(const std::string& key) = 0;

    // Atomically retrieves and deletes a value.
    virtual std::optional<std::string> getdel(const std::string& key) {
        return get(key);
    }
};

#endif
