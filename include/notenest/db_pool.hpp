#ifndef DB_POOL_HPP
#define DB_POOL_HPP

#include <postgresql/libpq-fe.h>

#include <memory>
#include <notenest/pool.hpp>
#include <string>

// Enum representing write or read database pool operation mode.
enum class DBPoolMode { WRITE, READ };

// Custom bounded database connection pool with read/write splitting, CircuitBreaker, and health
// checks.
class DBPool {
public:
    static DBPool& getInstance();

    // Initializes write and read connection pools with connection strings and pool size.
    void init(const std::string& write_conn_str, const std::string& read_conn_str,
              size_t pool_size);

    // Initializes both pools with the same connection string for backward compatibility.
    void init(const std::string& conn_str, size_t pool_size);

    // Acquires connection based on mode, tracking the actual pool acquired from.
    PGconn* acquire(DBPoolMode mode, DBPoolMode& acquired_mode);

    // Acquires connection defaulting to WRITE mode.
    PGconn* acquire(DBPoolMode mode = DBPoolMode::WRITE);

    // Releases connection back to the specified pool mode.
    void release(PGconn* conn, DBPoolMode mode = DBPoolMode::WRITE);

    // Closes all connection pools.
    void close();

    // Returns active connections count for specified pool mode.
    size_t getActiveCount(DBPoolMode mode = DBPoolMode::WRITE) const;

    // Returns idle connections count for specified pool mode.
    size_t getIdleCount(DBPoolMode mode = DBPoolMode::WRITE) const;

    // Returns circuit breaker state name for specified pool mode.
    std::string getCircuitState(DBPoolMode mode = DBPoolMode::WRITE) const;

    ~DBPool();

private:
    DBPool() = default;
    DBPool(const DBPool&) = delete;
    DBPool& operator=(const DBPool&) = delete;

    std::string write_conn_str_;
    std::string read_conn_str_;
    std::unique_ptr<Pool<PGconn>> write_pool_;
    std::unique_ptr<Pool<PGconn>> read_pool_;
};

// RAII Guard managing connection acquisition and automatic release with mode fallback.
class DBConnectionGuard {
public:
    explicit DBConnectionGuard(DBPoolMode mode = DBPoolMode::READ) {
        conn_ = DBPool::getInstance().acquire(mode, acquired_mode_);
    }
    ~DBConnectionGuard() {
        if (conn_) {
            DBPool::getInstance().release(conn_, acquired_mode_);
        }
    }
    PGconn* get() const {
        return conn_;
    }
    DBPoolMode getAcquiredMode() const {
        return acquired_mode_;
    }

private:
    PGconn* conn_{nullptr};
    DBPoolMode acquired_mode_{DBPoolMode::WRITE};
};

#endif
