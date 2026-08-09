#ifndef POOL_HPP
#define POOL_HPP

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <notenest/circuit_breaker.hpp>
#include <print>
#include <queue>
#include <stdexcept>
#include <string>

// Item wrapper for pooled resources tracking age and idle time.
template <typename T>
struct PooledItem {
    T* resource;
    std::chrono::steady_clock::time_point created_at;
    std::chrono::steady_clock::time_point last_used_at;
};

// Generic connection pool with health checks, max-age, idle timeout, and circuit breaker.
template <typename T>
class Pool {
public:
    using Factory = std::function<T*()>;
    using Destructor = std::function<void(T*)>;
    using HealthCheck = std::function<bool(T*)>;

    // Constructs generic connection pool with specified policies and factory functions.
    Pool(Factory factory, Destructor destructor, HealthCheck health_check, size_t max_size,
         std::chrono::milliseconds max_age = std::chrono::milliseconds(600000),
         std::chrono::milliseconds idle_timeout = std::chrono::milliseconds(60000))
        : factory_(std::move(factory)),
          destructor_(std::move(destructor)),
          health_check_(std::move(health_check)),
          max_size_(max_size),
          max_age_(max_age),
          idle_timeout_(idle_timeout),
          circuit_breaker_(5, std::chrono::milliseconds(5000)) {}

    ~Pool() {
        close();
    }

    // Pre-populates the connection pool up to target size.
    void init(size_t initial_size) {
        std::unique_lock<std::mutex> lock(mutex_);
        for (size_t i = 0; i < initial_size && i < max_size_; ++i) {
            T* resource = nullptr;
            try {
                resource = factory_();
            } catch (...) {
                resource = nullptr;
            }
            if (resource && health_check_(resource)) {
                auto now = std::chrono::steady_clock::now();
                pool_.push({resource, now, now});
                created_count_++;
            } else {
                if (resource)
                    destructor_(resource);
            }
        }
        if (!pool_.empty()) {
            circuit_breaker_.recordSuccess();
        }
    }

    // Acquires a connection from the pool, creating or validating connections as needed.
    T* acquire(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        if (!circuit_breaker_.allowRequest()) {
            std::println(stderr, "[Pool] Circuit breaker is OPEN. Fast failing request.");
            throw std::runtime_error("Circuit breaker is OPEN - pool unavailable");
        }

        auto start_time = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> lock(mutex_);

        while (pool_.empty()) {
            if (closed_) {
                throw std::runtime_error("Pool is closed");
            }
            if (created_count_ < max_size_) {
                // Create new resource on demand
                lock.unlock();
                T* resource = nullptr;
                try {
                    resource = factory_();
                } catch (...) {
                    resource = nullptr;
                }
                lock.lock();

                if (resource && health_check_(resource)) {
                    created_count_++;
                    active_count_++;
                    total_acquisitions_++;
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start_time);
                    total_wait_time_ms_ += elapsed.count();
                    circuit_breaker_.recordSuccess();
                    return resource;
                } else {
                    if (resource)
                        destructor_(resource);
                    circuit_breaker_.recordFailure();
                    throw std::runtime_error("Failed to create healthy pooled connection");
                }
            }

            if (cv_.wait_for(lock, timeout) == std::cv_status::timeout) {
                circuit_breaker_.recordFailure();
                throw std::runtime_error("Pool acquisition timeout");
            }
        }

        // Pop available pooled connection and check health/max-age/idle timeout
        while (!pool_.empty()) {
            PooledItem<T> item = pool_.front();
            pool_.pop();

            auto now = std::chrono::steady_clock::now();
            bool is_expired =
                (now - item.created_at > max_age_) || (now - item.last_used_at > idle_timeout_);

            if (!is_expired && health_check_(item.resource)) {
                active_count_++;
                total_acquisitions_++;
                auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time);
                total_wait_time_ms_ += elapsed.count();
                circuit_breaker_.recordSuccess();
                return item.resource;
            } else {
                // Evict stale or broken connection
                destructor_(item.resource);
                created_count_--;
                eviction_count_++;
                std::println("[Pool] Evicted connection (expired={}, healthy={})", is_expired,
                             !is_expired);
            }
        }

        // If all popped connections were evicted, attempt creating a fresh connection
        lock.unlock();
        T* resource = nullptr;
        try {
            resource = factory_();
        } catch (...) {
            resource = nullptr;
        }
        lock.lock();

        if (resource && health_check_(resource)) {
            created_count_++;
            active_count_++;
            total_acquisitions_++;
            circuit_breaker_.recordSuccess();
            return resource;
        } else {
            if (resource)
                destructor_(resource);
            circuit_breaker_.recordFailure();
            throw std::runtime_error("Failed to re-establish pooled connection after evictions");
        }
    }

    // Releases a connection back to the pool.
    void release(T* resource) {
        if (!resource)
            return;
        std::unique_lock<std::mutex> lock(mutex_);
        if (closed_) {
            destructor_(resource);
            created_count_--;
            return;
        }

        if (active_count_ > 0)
            active_count_--;
        auto now = std::chrono::steady_clock::now();

        if (health_check_(resource)) {
            pool_.push({resource, now, now});
            circuit_breaker_.recordSuccess();
        } else {
            destructor_(resource);
            created_count_--;
            eviction_count_++;
            circuit_breaker_.recordFailure();
        }
        cv_.notify_one();
    }

    // Closes the pool and destroys all pooled connections.
    void close() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (closed_)
            return;
        closed_ = true;

        while (!pool_.empty()) {
            PooledItem<T> item = pool_.front();
            pool_.pop();
            destructor_(item.resource);
        }
        created_count_ = 0;
        active_count_ = 0;
        cv_.notify_all();
    }

    // Returns number of active connections in use.
    size_t getActiveCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_count_;
    }

    // Returns number of idle connections in pool.
    size_t getIdleCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pool_.size();
    }

    // Returns total evictions count.
    size_t getEvictionCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return eviction_count_;
    }

    // Returns total acquisitions count.
    size_t getTotalAcquisitions() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return total_acquisitions_;
    }

    // Returns circuit breaker state name.
    std::string getCircuitState() const {
        return circuit_breaker_.getStateName();
    }

    // Directly triggers a failure notification to the circuit breaker.
    void recordFailure() {
        circuit_breaker_.recordFailure();
    }

private:
    Factory factory_;
    Destructor destructor_;
    HealthCheck health_check_;
    size_t max_size_;
    std::chrono::milliseconds max_age_;
    std::chrono::milliseconds idle_timeout_;
    CircuitBreaker circuit_breaker_;

    std::queue<PooledItem<T>> pool_;
    size_t created_count_ = 0;
    size_t active_count_ = 0;
    size_t eviction_count_ = 0;
    size_t total_acquisitions_ = 0;
    uint64_t total_wait_time_ms_ = 0;
    bool closed_ = false;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
};

// RAII Guard template for automatic pool resource acquisition and release.
template <typename T>
class PoolGuard {
public:
    PoolGuard(Pool<T>& pool) : pool_(pool), resource_(pool.acquire()) {}
    ~PoolGuard() {
        if (resource_) {
            pool_.release(resource_);
        }
    }
    T* get() const {
        return resource_;
    }

private:
    Pool<T>& pool_;
    T* resource_;
};

#endif
