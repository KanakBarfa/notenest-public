#include "notenest/outbox_relay.hpp"

#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace notenest {

namespace {
// Releases pooled connections on every path, including exceptions.
struct ConnGuard {
    DBPool& pool;
    PGconn* conn;
    ~ConnGuard() {
        if (conn != nullptr) {
            pool.release(conn);
        }
    }
};

bool check(PGresult* res, ExecStatusType expected, const char* what) {
    bool ok = res != nullptr && PQresultStatus(res) == expected;
    if (!ok) {
        std::cerr << "[OutboxRelay] " << what << " failed\n";
    }
    if (res != nullptr) {
        PQclear(res);
    }
    return ok;
}
}  // namespace

OutboxRelay::OutboxRelay(DBPool& db_pool, KafkaProducer& kafka_producer)
    : db_pool_(db_pool), kafka_producer_(kafka_producer) {}

OutboxRelay::~OutboxRelay() {
    stop();
}

void OutboxRelay::start() {
    if (running_)
        return;
    running_ = true;
    thread_ = std::thread(&OutboxRelay::run, this);
    std::cout << "[OutboxRelay] Background thread started.\n";
}

void OutboxRelay::stop() {
    if (!running_)
        return;
    running_ = false;
    if (thread_.joinable()) {
        thread_.join();
    }
    std::cout << "[OutboxRelay] Background thread stopped.\n";
}

void OutboxRelay::run() {
    int iterations = 0;
    while (running_) {
        try {
            PGconn* conn = nullptr;
            try {
                conn = db_pool_.acquire();
            } catch (const std::exception&) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            ConnGuard guard{db_pool_, conn};

            // BEGIN
            {
                PGresult* begin_res = PQexec(conn, "BEGIN");
                if (!check(begin_res, PGRES_COMMAND_OK, "BEGIN")) {
                    continue;
                }
            }

            // Claim a batch.
            const char* query =
                "SELECT id::text, event_type, payload::text FROM outbox "
                "WHERE published = FALSE ORDER BY created_at ASC LIMIT 100 FOR UPDATE SKIP LOCKED";
            std::vector<std::pair<std::string, std::pair<std::string, std::string>>> batch;
            {
                PGresult* res = PQexec(conn, query);
                bool ok = res != nullptr && PQresultStatus(res) == PGRES_TUPLES_OK;
                if (!ok) {
                    std::cerr << "[OutboxRelay] Claim select failed: "
                              << (PQerrorMessage(conn) ? PQerrorMessage(conn) : "?") << "\n";
                    PQclear(res);
                    check(PQexec(conn, "ROLLBACK"), PGRES_COMMAND_OK, "ROLLBACK");
                    continue;
                }
                for (int i = 0; i < PQntuples(res); ++i) {
                    batch.emplace_back(
                        PQgetvalue(res, i, 0),
                        std::make_pair(PQgetvalue(res, i, 1), PQgetvalue(res, i, 2)));
                }
                PQclear(res);
            }

            // Produce and wait for delivery reports before committing the mark.
            std::vector<std::string> produced_ids;
            for (const auto& [id, ev] : batch) {
                if (kafka_producer_.produce(ev.first, id, ev.second)) {
                    produced_ids.push_back(id);
                } else {
                    std::cerr << "[OutboxRelay] Enqueue failed for outbox id " << id << "\n";
                }
            }

            std::vector<std::string> confirmed;
            if (!produced_ids.empty()) {
                kafka_producer_.flushAll(10000);
                for (const auto& id : produced_ids) {
                    if (kafka_producer_.wasDelivered(id)) {
                        confirmed.push_back(id);
                    } else {
                        std::cerr << "[OutboxRelay] No delivery report for outbox id " << id
                                  << "; leaving unpublished for retry\n";
                    }
                }

                // Parameterized mark; only broker-confirmed rows flip to TRUE.
                if (!confirmed.empty()) {
                    std::string ids = "{";
                    for (size_t i = 0; i < confirmed.size(); ++i) {
                        if (i > 0)
                            ids += ",";
                        ids += "\"" + confirmed[i] + "\"";
                    }
                    ids += "}";
                    const char* update_sql =
                        "UPDATE outbox SET published = TRUE WHERE id = ANY($1::uuid[])";
                    const char* params[1] = {ids.c_str()};
                    PGresult* update_res =
                        PQexecParams(conn, update_sql, 1, nullptr, params, nullptr, nullptr, 0);
                    if (!check(update_res, PGRES_COMMAND_OK, "UPDATE")) {
                        check(PQexec(conn, "ROLLBACK"), PGRES_COMMAND_OK, "ROLLBACK");
                        continue;
                    }
                }
            }

            if (!check(PQexec(conn, "COMMIT"), PGRES_COMMAND_OK, "COMMIT")) {
                continue;
            }

            // Periodic retention: purge long-published rows.
            if (++iterations % kRetentionEveryLoops == 0) {
                const char* retention_sql =
                    "DELETE FROM outbox WHERE published = TRUE AND created_at < NOW() - ($1 || ' "
                    "days')::interval";
                std::string days = std::to_string(kRetentionDays);
                const char* params[1] = {days.c_str()};
                PGresult* retention_res =
                    PQexecParams(conn, retention_sql, 1, nullptr, params, nullptr, nullptr, 0);
                check(retention_res, PGRES_COMMAND_OK, "Retention delete");
            }
        } catch (const std::exception& e) {
            std::cerr << "[OutboxRelay] Exception in relay loop: " << e.what() << "\n";
        }

        kafka_producer_.poll(0);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

}  // namespace notenest
