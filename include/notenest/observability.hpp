#ifndef OBSERVABILITY_HPP
#define OBSERVABILITY_HPP

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Observability metrics collector and structured JSON logger
class Observability {
public:
    static Observability& getInstance();

    void incRequest(const std::string& method, const std::string& path, int status);

    void observeDuration(const std::string& method, double seconds);

    void setActiveWebSockets(int count);

    void setKafkaLag(int lag);

    std::string renderMetrics();

    static std::string generateTraceId();

    static std::string generateSpanId();

    static std::pair<std::string, std::string> parseTraceparent(const std::string& header);

    static std::string formatTraceparent(const std::string& trace_id, const std::string& span_id);

    static void logJson(const std::string& level, const std::string& msg,
                        const std::string& trace_id = "", const std::string& span_id = "");

    static void sendOtlpSpan(const std::string& span_name, const std::string& trace_id,
                             const std::string& span_id, const std::string& parent_id,
                             uint64_t start_ns, uint64_t end_ns);

private:
    Observability() = default;

    std::mutex mutex_;
    std::unordered_map<std::string, uint64_t> request_counts_;
    std::unordered_map<std::string, uint64_t> duration_buckets_;
    double duration_sum_ = 0.0;
    uint64_t duration_count_ = 0;
    std::atomic<int> active_websockets_{0};
    std::atomic<int> kafka_lag_{0};
};

#endif
