#include "notenest/kafka_producer.hpp"
#if __has_include(<rdkafkacpp.h>)
#include <rdkafkacpp.h>
#else
#include <librdkafka/rdkafkacpp.h>
#endif
#include <iostream>

namespace notenest {

KafkaProducer::KafkaProducer(const std::string& brokers) {
    if (brokers.empty()) {
        std::cerr << "[KafkaProducer] No brokers specified, skipping initialization.\n";
        return;
    }

    std::string errstr;
    std::unique_ptr<RdKafka::Conf> conf(RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));
    if (!conf) {
        std::cerr << "[KafkaProducer] Failed to create RdKafka global config.\n";
        return;
    }

    if (conf->set("bootstrap.servers", brokers, errstr) != RdKafka::Conf::CONF_OK) {
        std::cerr << "[KafkaProducer] Failed to set bootstrap.servers: " << errstr << "\n";
        return;
    }

    conf->set("acks", "all", errstr);
    conf->set("retries", "5", errstr);

    RdKafka::Producer* producer = RdKafka::Producer::create(conf.get(), errstr);
    if (!producer) {
        std::cerr << "[KafkaProducer] Failed to create producer: " << errstr << "\n";
        return;
    }

    producer_.reset(producer);
    initialized_ = true;
    std::cout << "[KafkaProducer] Initialized with brokers: " << brokers << "\n";
}

KafkaProducer::~KafkaProducer() {
    if (producer_) {
        producer_->flush(2000);
    }
}

bool KafkaProducer::produce(const std::string& topic_name, const std::string& key,
                            const std::string& payload) {
    if (!initialized_ || !producer_) {
        std::cerr << "[KafkaProducer] Producer not initialized.\n";
        return false;
    }

    RdKafka::ErrorCode err = producer_->produce(
        topic_name, RdKafka::Topic::PARTITION_UA, RdKafka::Producer::RK_MSG_COPY,
        const_cast<char*>(payload.data()), payload.size(), key.data(), key.size(), 0, nullptr);

    if (err != RdKafka::ERR_NO_ERROR) {
        std::cerr << "[KafkaProducer] Produce failed for topic " << topic_name << ": "
                  << RdKafka::err2str(err) << "\n";
        return false;
    }

    producer_->poll(0);
    return true;
}

void KafkaProducer::poll(int timeout_ms) {
    if (producer_) {
        producer_->poll(timeout_ms);
    }
}

}  // namespace notenest
