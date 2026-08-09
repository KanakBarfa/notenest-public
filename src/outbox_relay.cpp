#include "notenest/outbox_relay.hpp"

#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>
#include <vector>

namespace notenest {

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
    while (running_) {
        try {
            PGconn* conn = nullptr;
            try {
                conn = db_pool_.acquire();
            } catch (const std::exception& e) {
                // DB pool temporarily unavailable or circuit broken
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            PGresult* begin_res = PQexec(conn, "BEGIN");
            PQclear(begin_res);

            const char* query =
                "SELECT id::text, event_type, payload::text FROM outbox "
                "WHERE published = FALSE ORDER BY created_at ASC LIMIT 100 FOR UPDATE SKIP LOCKED";
            PGresult* res = PQexec(conn, query);

            if (PQresultStatus(res) == PGRES_TUPLES_OK) {
                int rows = PQntuples(res);
                std::vector<std::string> published_ids;

                for (int i = 0; i < rows; ++i) {
                    std::string id = PQgetvalue(res, i, 0);
                    std::string event_type = PQgetvalue(res, i, 1);
                    std::string payload_str = PQgetvalue(res, i, 2);

                    // Publish event to topic (topic name = event_type, e.g. "note.events")
                    bool success = kafka_producer_.produce(event_type, id, payload_str);
                    if (success) {
                        published_ids.push_back(id);
                    }
                }

                PQclear(res);

                if (!published_ids.empty()) {
                    std::string update_sql = "UPDATE outbox SET published = TRUE WHERE id IN (";
                    for (size_t i = 0; i < published_ids.size(); ++i) {
                        update_sql += "'" + published_ids[i] + "'";
                        if (i + 1 < published_ids.size())
                            update_sql += ", ";
                    }
                    update_sql += ")";

                    PGresult* update_res = PQexec(conn, update_sql.c_str());
                    PQclear(update_res);
                }

                PGresult* commit_res = PQexec(conn, "COMMIT");
                PQclear(commit_res);
            } else {
                PQclear(res);
                PGresult* rollback_res = PQexec(conn, "ROLLBACK");
                PQclear(rollback_res);
            }

            db_pool_.release(conn);
        } catch (const std::exception& e) {
            std::cerr << "[OutboxRelay] Exception in relay loop: " << e.what() << "\n";
        }

        kafka_producer_.poll(0);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

}  // namespace notenest
