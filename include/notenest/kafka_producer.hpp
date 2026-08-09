#ifndef NOTENEST_KAFKA_PRODUCER_HPP
#define NOTENEST_KAFKA_PRODUCER_HPP

#include <memory>
#include <string>

namespace RdKafka {
class Producer;
}  // namespace RdKafka

namespace notenest {

// Thread safe Kafka producer wrapper
class KafkaProducer {
public:
    explicit KafkaProducer(const std::string& brokers);
    ~KafkaProducer();

    KafkaProducer(const KafkaProducer&) = delete;
    KafkaProducer& operator=(const KafkaProducer&) = delete;

    // Produce message to topic with key and payload
    bool produce(const std::string& topic_name, const std::string& key, const std::string& payload);

    // Poll delivery reports
    void poll(int timeout_ms = 0);

private:
    std::unique_ptr<RdKafka::Producer> producer_;
    bool initialized_{false};
};

}  // namespace notenest

#endif  // NOTENEST_KAFKA_PRODUCER_HPP
