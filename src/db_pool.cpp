#include <notenest/db_pool.hpp>
#include <print>
#include <stdexcept>

DBPool& DBPool::getInstance() {
    static DBPool instance;
    return instance;
}

void DBPool::init(const std::string& write_conn_str, const std::string& read_conn_str,
                  size_t pool_size) {
    write_conn_str_ = write_conn_str;
    read_conn_str_ = read_conn_str;

    auto make_pool = [](const std::string& conn_str, size_t size,
                        const char* name) -> std::unique_ptr<Pool<PGconn>> {
        auto factory = [conn_str, name]() -> PGconn* {
            PGconn* conn = PQconnectdb(conn_str.c_str());
            if (PQstatus(conn) != CONNECTION_OK) {
                std::println(stderr, "[DBPool-{}] Connection failed: {}", name,
                             PQerrorMessage(conn));
                if (conn)
                    PQfinish(conn);
                return nullptr;
            }
            return conn;
        };

        auto destructor = [](PGconn* conn) {
            if (conn)
                PQfinish(conn);
        };

        auto health_check = [](PGconn* conn) -> bool {
            if (!conn)
                return false;
            if (PQstatus(conn) != CONNECTION_OK) {
                PQreset(conn);
            }
            return PQstatus(conn) == CONNECTION_OK;
        };

        auto p = std::make_unique<Pool<PGconn>>(factory, destructor, health_check, size);
        p->init(size);
        std::println("[DBPool-{}] Initialized pool with target size {}", name, size);
        return p;
    };

    write_pool_ = make_pool(write_conn_str_, pool_size, "WRITE");
    if (!read_conn_str_.empty() && read_conn_str_ != write_conn_str_) {
        read_pool_ = make_pool(read_conn_str_, pool_size, "READ");
    } else {
        read_pool_ = nullptr;
    }
}

void DBPool::init(const std::string& conn_str, size_t pool_size) {
    init(conn_str, conn_str, pool_size);
}

PGconn* DBPool::acquire(DBPoolMode mode, DBPoolMode& acquired_mode) {
    if (mode == DBPoolMode::READ && read_pool_) {
        try {
            PGconn* conn = read_pool_->acquire();
            if (conn) {
                acquired_mode = DBPoolMode::READ;
                return conn;
            }
        } catch (const std::exception& e) {
            std::println(stderr,
                         "[DBPool] Read pool acquire failed, falling back to WRITE pool: {}",
                         e.what());
        }
    }

    if (!write_pool_) {
        throw std::runtime_error("DBPool write_pool_ not initialized");
    }

    PGconn* conn = write_pool_->acquire();
    acquired_mode = DBPoolMode::WRITE;
    return conn;
}

PGconn* DBPool::acquire(DBPoolMode mode) {
    DBPoolMode dummy;
    return acquire(mode, dummy);
}

void DBPool::release(PGconn* conn, DBPoolMode mode) {
    if (mode == DBPoolMode::READ && read_pool_) {
        read_pool_->release(conn);
    } else if (write_pool_) {
        write_pool_->release(conn);
    } else if (conn) {
        PQfinish(conn);
    }
}

void DBPool::close() {
    if (read_pool_) {
        read_pool_->close();
    }
    if (write_pool_) {
        write_pool_->close();
    }
}

size_t DBPool::getActiveCount(DBPoolMode mode) const {
    if (mode == DBPoolMode::READ && read_pool_)
        return read_pool_->getActiveCount();
    return write_pool_ ? write_pool_->getActiveCount() : 0;
}

size_t DBPool::getIdleCount(DBPoolMode mode) const {
    if (mode == DBPoolMode::READ && read_pool_)
        return read_pool_->getIdleCount();
    return write_pool_ ? write_pool_->getIdleCount() : 0;
}

std::string DBPool::getCircuitState(DBPoolMode mode) const {
    if (mode == DBPoolMode::READ && read_pool_)
        return read_pool_->getCircuitState();
    return write_pool_ ? write_pool_->getCircuitState() : "CLOSED";
}

DBPool::~DBPool() {
    close();
}
