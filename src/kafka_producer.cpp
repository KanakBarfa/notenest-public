#include "notenest/kafka_producer.hpp"
#if __has_include(<rdkafkacpp.h>)
#include <rdkafkacpp.h>
#else
#include <librdkafka/rdkafkacpp.h>
#endif
#include <iostream>

namespace notenest {

// Forwards async delivery reports into the producer's result map.
class DeliveryReportBridge : public RdKafka::DeliveryReportCb {
public:
    explicit DeliveryReportBridge(KafkaProducer* owner) : owner_(owner) {}
    void dr_cb(RdKafka::Message& msg) override {
        std::string key = msg.key() ? *(msg.key()) : "";
        owner_->recordDelivery(key, static_cast<int>(msg.err()));
    }

private:
    KafkaProducer* owner_;
};

KafkaProducer::KafkaProducer(const std::string& brokers) {
    if (brokers.empty()) {
        std::cerr << "[KafkaProducer] No brokers specified, skipping initialization.\n";
        return;
    }

    dr_sink_ = [this](const std::string& key, int err) { recordDelivery(key, err); };

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
    if (conf->set("enable.idempotence", "true", errstr) != RdKafka::Conf::CONF_OK) {
        std::cerr << "[KafkaProducer] Idempotent producer unavailable: " << errstr << "\n";
    }

    conf->set("acks", "all", errstr);
    conf->set("retries", "5", errstr);

    auto bridge = std::make_unique<DeliveryReportBridge>(this);
    if (conf->set("dr_cb", bridge.get(), errstr) != RdKafka::Conf::CONF_OK) {
        std::cerr << "[KafkaProducer] Failed to set delivery report callback: " << errstr << "\n";
    } else {
        dr_bridge_ = bridge.release();
    }

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
    delete static_cast<DeliveryReportBridge*>(dr_bridge_);
}

void KafkaProducer::recordDelivery(const std::string& key, int err) {
    if (key.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(dr_mu_);
    delivery_results_[key] = err;
}

bool KafkaProducer::flushAll(int timeout_ms) {
    if (!initialized_ || !producer_) {
        return false;
    }
    // flush() pumps the event loop, which fires the delivery-report callbacks.
    RdKafka::ErrorCode err = producer_->flush(timeout_ms);
    return err == RdKafka::ERR_NO_ERROR;
}

bool KafkaProducer::wasDelivered(const std::string& key) {
    std::lock_guard<std::mutex> lock(dr_mu_);
    auto it = delivery_results_.find(key);
    if (it == delivery_results_.end()) {
        return false;  // no report yet: treat as undelivered
    }
    bool ok = it->second == static_cast<int>(RdKafka::ERR_NO_ERROR);
    delivery_results_.erase(it);
    return ok;
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
