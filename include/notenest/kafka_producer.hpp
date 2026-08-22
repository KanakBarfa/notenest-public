#ifndef NOTENEST_KAFKA_PRODUCER_HPP
#define NOTENEST_KAFKA_PRODUCER_HPP

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace RdKafka {
class Producer;
}  // namespace RdKafka

namespace notenest {

// Forwards async delivery reports into KafkaProducer; defined in kafka_producer.cpp.
class DeliveryReportBridge;

// Thread safe Kafka producer wrapper with delivery-report confirmation.
class KafkaProducer {
public:
    explicit KafkaProducer(const std::string& brokers);
    ~KafkaProducer();

    KafkaProducer(const KafkaProducer&) = delete;
    KafkaProducer& operator=(const KafkaProducer&) = delete;

    // Produce message to topic with key and payload
    bool produce(const std::string& topic_name, const std::string& key, const std::string& payload);

    // Pumps delivery reports until the queue drains or timeout expires.
    bool flushAll(int timeout_ms);

    // True when a delivery report confirmed this key; consumes the record.
    bool wasDelivered(const std::string& key);

    // Poll delivery reports
    void poll(int timeout_ms = 0);

private:
    friend class DeliveryReportBridge;
    void recordDelivery(const std::string& key, int err);

    std::unique_ptr<RdKafka::Producer> producer_;
    bool initialized_{false};

    void* dr_bridge_ = nullptr;  // DeliveryReportBridge instance, owned here
    std::function<void(const std::string&, int)> dr_sink_;
    std::mutex dr_mu_;
    std::unordered_map<std::string, int> delivery_results_;
};

}  // namespace notenest

#endif  // NOTENEST_KAFKA_PRODUCER_HPP
