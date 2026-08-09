#ifndef REDIS_CACHE_HPP
#define REDIS_CACHE_HPP

#include <hiredis/hiredis.h>

#include <memory>
#include <notenest/cache.hpp>
#include <notenest/pool.hpp>
#include <optional>
#include <string>

// Redis implementation of Cache interface with connection pool and circuit breaker.
class RedisCache : public Cache {
public:
    RedisCache(const std::string& host, int port, size_t pool_size = 10);
    ~RedisCache() override;

    // Retrieve value from Redis.
    std::optional<std::string> get(const std::string& key) override;

    // Set value in Redis with TTL.
    void set(const std::string& key, const std::string& value, int ttl_seconds) override;

    // Delete value from Redis.
    void del(const std::string& key) override;

    // Returns active connection count.
    size_t getActiveCount() const;

    // Returns idle connection count.
    size_t getIdleCount() const;

    // Returns circuit breaker state name.
    std::string getCircuitState() const;

private:
    std::string host_;
    int port_;
    std::unique_ptr<Pool<redisContext>> pool_;
};

#endif
