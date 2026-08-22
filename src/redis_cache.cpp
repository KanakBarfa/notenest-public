#include <notenest/redis_cache.hpp>
#include <print>

RedisCache::RedisCache(const std::string& host, int port, size_t pool_size)
    : host_(host), port_(port) {
    auto factory = [this]() -> redisContext* {
        struct timeval timeout = {1, 500000};
        redisContext* ctx = redisConnectWithTimeout(host_.c_str(), port_, timeout);
        if (!ctx || ctx->err) {
            std::println(stderr, "[RedisCache] Connection error: {}",
                         ctx ? ctx->errstr : "Allocation failed");
            if (ctx)
                redisFree(ctx);
            return nullptr;
        }
        return ctx;
    };

    auto destructor = [](redisContext* ctx) {
        if (ctx)
            redisFree(ctx);
    };

    auto health_check = [](redisContext* ctx) -> bool {
        if (!ctx || ctx->err)
            return false;
        redisReply* reply = (redisReply*)redisCommand(ctx, "PING");
        if (!reply)
            return false;
        bool ok = (reply->type == REDIS_REPLY_STATUS && std::string(reply->str) == "PONG");
        freeReplyObject(reply);
        return ok;
    };

    pool_ = std::make_unique<Pool<redisContext>>(factory, destructor, health_check, pool_size);
    pool_->init(pool_size);
    std::println("[RedisCache] Initialized connection pool with target size {}", pool_size);
}

RedisCache::~RedisCache() {
    if (pool_) {
        pool_->close();
    }
}

std::optional<std::string> RedisCache::get(const std::string& key) {
    try {
        PoolGuard<redisContext> guard(*pool_);
        redisContext* ctx = guard.get();
        if (!ctx)
            return std::nullopt;

        redisReply* reply = (redisReply*)redisCommand(ctx, "GET %s", key.c_str());
        if (!reply) {
            pool_->recordFailure();
            return std::nullopt;
        }

        std::optional<std::string> result = std::nullopt;
        if (reply->type == REDIS_REPLY_STRING) {
            result = std::string(reply->str, reply->len);
        }
        freeReplyObject(reply);
        return result;
    } catch (const std::exception& e) {
        std::println(stderr, "[RedisCache] GET failed: {}", e.what());
        return std::nullopt;
    }
}

void RedisCache::set(const std::string& key, const std::string& value, int ttl_seconds) {
    try {
        PoolGuard<redisContext> guard(*pool_);
        redisContext* ctx = guard.get();
        if (!ctx)
            return;

        redisReply* reply = (redisReply*)redisCommand(ctx, "SET %s %b EX %d", key.c_str(),
                                                      value.c_str(), value.size(), ttl_seconds);
        if (!reply) {
            pool_->recordFailure();
            return;
        }
        freeReplyObject(reply);
    } catch (const std::exception& e) {
        std::println(stderr, "[RedisCache] SET failed: {}", e.what());
    }
}

void RedisCache::del(const std::string& key) {
    try {
        PoolGuard<redisContext> guard(*pool_);
        redisContext* ctx = guard.get();
        if (!ctx)
            return;

        redisReply* reply = (redisReply*)redisCommand(ctx, "DEL %s", key.c_str());
        if (!reply) {
            pool_->recordFailure();
            return;
        }
        freeReplyObject(reply);
    } catch (const std::exception& e) {
        std::println(stderr, "[RedisCache] DEL failed: {}", e.what());
    }
}

std::optional<std::string> RedisCache::getdel(const std::string& key) {
    try {
        PoolGuard<redisContext> guard(*pool_);
        redisContext* ctx = guard.get();
        if (!ctx)
            return std::nullopt;

        // Atomic single-use redemption (Redis >= 6.2).
        redisReply* reply = (redisReply*)redisCommand(ctx, "GETDEL %s", key.c_str());
        if (!reply) {
            pool_->recordFailure();
            return std::nullopt;
        }

        std::optional<std::string> result = std::nullopt;
        if (reply->type == REDIS_REPLY_STRING) {
            result = std::string(reply->str, reply->len);
        } else if (reply->type == REDIS_REPLY_ERROR &&
                   std::string(reply->str).find("unknown command") != std::string::npos) {
            freeReplyObject(reply);
            result = get(key);
            del(key);
            return result;
        }
        freeReplyObject(reply);
        return result;
    } catch (const std::exception& e) {
        std::println(stderr, "[RedisCache] GETDEL failed: {}", e.what());
        return std::nullopt;
    }
}

size_t RedisCache::getActiveCount() const {
    return pool_ ? pool_->getActiveCount() : 0;
}

size_t RedisCache::getIdleCount() const {
    return pool_ ? pool_->getIdleCount() : 0;
}

std::string RedisCache::getCircuitState() const {
    return pool_ ? pool_->getCircuitState() : "CLOSED";
}
