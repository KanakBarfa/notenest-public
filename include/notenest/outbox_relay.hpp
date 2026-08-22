#ifndef NOTENEST_OUTBOX_RELAY_HPP
#define NOTENEST_OUTBOX_RELAY_HPP

#include <atomic>
#include <thread>

#include "notenest/db_pool.hpp"
#include "notenest/kafka_producer.hpp"

namespace notenest {

// Background relay thread tailing outbox table and producing to Kafka
class OutboxRelay {
public:
    OutboxRelay(DBPool& db_pool, KafkaProducer& kafka_producer);
    ~OutboxRelay();

    OutboxRelay(const OutboxRelay&) = delete;
    OutboxRelay& operator=(const OutboxRelay&) = delete;

    void start();
    void stop();

private:
    void run();

    static constexpr int kRetentionDays = 7;
    static constexpr int kRetentionEveryLoops = 7200;  // ~hourly at 500ms cadence

    DBPool& db_pool_;
    KafkaProducer& kafka_producer_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace notenest

#endif  // NOTENEST_OUTBOX_RELAY_HPP
