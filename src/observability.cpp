#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <notenest/observability.hpp>
#include <random>
#include <sstream>
#include <thread>

Observability& Observability::getInstance() {
    static Observability instance;
    return instance;
}

void Observability::incRequest(const std::string& method, const std::string& path, int status) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string key = method + "|" + path + "|" + std::to_string(status);
    request_counts_[key]++;
}

void Observability::observeDuration(const std::string& method, double seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    duration_sum_ += seconds;
    duration_count_++;

    std::vector<double> buckets = {0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0};
    for (double b : buckets) {
        if (seconds <= b) {
            std::string bucket_key = method + "|" + std::to_string(b);
            duration_buckets_[bucket_key]++;
        }
    }
}

void Observability::setActiveWebSockets(int count) {
    active_websockets_.store(count);
}

void Observability::setKafkaLag(int lag) {
    kafka_lag_.store(lag);
}

std::string Observability::renderMetrics() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream ss;

    ss << "# HELP http_requests_total Total HTTP and gRPC requests\n";
    ss << "# TYPE http_requests_total counter\n";
    if (request_counts_.empty()) {
        ss << "http_requests_total{service=\"app-service\",method=\"GET\",path=\"/"
              "\",status=\"200\"} 0\n";
    } else {
        for (const auto& [key, count] : request_counts_) {
            size_t p1 = key.find('|');
            size_t p2 = key.find('|', p1 + 1);
            std::string m = key.substr(0, p1);
            std::string p = key.substr(p1 + 1, p2 - p1 - 1);
            std::string s = key.substr(p2 + 1);
            ss << "http_requests_total{service=\"app-service\",method=\"" << m << "\",path=\"" << p
               << "\",status=\"" << s << "\"} " << count << "\n";
        }
    }

    ss << "# HELP http_request_duration_seconds Request duration in seconds\n";
    ss << "# TYPE http_request_duration_seconds histogram\n";
    std::vector<double> buckets = {0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0};
    for (double b : buckets) {
        uint64_t count = 0;
        for (const auto& [key, cnt] : duration_buckets_) {
            size_t p1 = key.find('|');
            double val = std::stod(key.substr(p1 + 1));
            if (val <= b) {
                count += cnt;
            }
        }
        ss << "http_request_duration_seconds_bucket{service=\"app-service\",method=\"HTTP\",le=\""
           << b << "\"} " << count << "\n";
    }
    ss << "http_request_duration_seconds_bucket{service=\"app-service\",method=\"HTTP\",le=\"+"
          "Inf\"} "
       << duration_count_ << "\n";
    ss << "http_request_duration_seconds_sum{service=\"app-service\",method=\"HTTP\"} "
       << duration_sum_ << "\n";
    ss << "http_request_duration_seconds_count{service=\"app-service\",method=\"HTTP\"} "
       << duration_count_ << "\n";

    ss << "# HELP notenest_active_websockets Active WebSocket connections\n";
    ss << "# TYPE notenest_active_websockets gauge\n";
    ss << "notenest_active_websockets " << active_websockets_.load() << "\n";

    ss << "# HELP kafka_consumer_lag Kafka consumer lag\n";
    ss << "# TYPE kafka_consumer_lag gauge\n";
    ss << "kafka_consumer_lag{topic=\"note.events\",group=\"notification-group\"} "
       << kafka_lag_.load() << "\n";

    return ss.str();
}

std::string Observability::generateTraceId() {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(16) << dist(rng) << std::setw(16) << dist(rng);
    return ss.str();
}

std::string Observability::generateSpanId() {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(16) << dist(rng);
    return ss.str();
}

std::pair<std::string, std::string> Observability::parseTraceparent(const std::string& header) {
    if (header.size() >= 55 && header.substr(0, 3) == "00-") {
        std::string trace_id = header.substr(3, 32);
        std::string parent_id = header.substr(36, 16);
        return {trace_id, parent_id};
    }
    if (!header.empty() && header.size() == 32) {
        return {header, generateSpanId()};
    }
    return {generateTraceId(), generateSpanId()};
}

std::string Observability::formatTraceparent(const std::string& trace_id,
                                             const std::string& span_id) {
    return "00-" + trace_id + "-" + span_id + "-01";
}

void Observability::logJson(const std::string& level, const std::string& msg,
                            const std::string& trace_id, const std::string& span_id) {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&in_time_t), "%Y-%m-%dT%H:%M:%SZ");

    std::string t_id = trace_id.empty() ? "00000000000000000000000000000000" : trace_id;
    std::string s_id = span_id.empty() ? "0000000000000000" : span_id;

    std::stringstream log_ss;
    log_ss << "{\"timestamp\":\"" << ss.str() << "\",\"level\":\"" << level
           << "\",\"service\":\"app-service\",\"trace_id\":\"" << t_id << "\",\"span_id\":\""
           << s_id << "\",\"message\":\"";
    for (char c : msg) {
        if (c == '"')
            log_ss << "\\\"";
        else if (c == '\\')
            log_ss << "\\\\";
        else if (c == '\n')
            log_ss << "\\n";
        else
            log_ss << c;
    }
    log_ss << "\"}\n";

    std::cout << log_ss.str() << std::flush;
}

void Observability::sendOtlpSpan(const std::string& span_name, const std::string& trace_id,
                                 const std::string& span_id, const std::string& parent_id,
                                 uint64_t start_ns, uint64_t end_ns) {
    std::thread([=]() {
        const char* otel_host_env = std::getenv("OTEL_COLLECTOR_HOST");
        std::string otel_host = otel_host_env ? otel_host_env : "otel-collector";
        const char* otel_port_env = std::getenv("OTEL_COLLECTOR_PORT");
        int otel_port = otel_port_env ? std::stoi(otel_port_env) : 4318;

        struct hostent* he = gethostbyname(otel_host.c_str());
        if (!he)
            return;

        int sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0)
            return;

        struct sockaddr_in serv_addr = {};
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(otel_port);
        std::memcpy(&serv_addr.sin_addr, he->h_addr_list[0], he->h_length);

        if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
            close(sockfd);
            return;
        }

        std::ostringstream body;
        body << "{\"resourceSpans\":[{\"resource\":{\"attributes\":[{\"key\":\"service.name\","
                "\"value\":{\"stringValue\":\"app-service\"}}]},\"scopeSpans\":[{\"spans\":[{"
                "\"traceId\":\""
             << trace_id << "\",\"spanId\":\"" << span_id << "\",\"parentSpanId\":\"" << parent_id
             << "\",\"name\":\"" << span_name << "\",\"kind\":1,\"startTimeUnixNano\":\""
             << start_ns << "\",\"endTimeUnixNano\":\"" << end_ns
             << "\",\"status\":{\"code\":1}}]}]}]}";

        std::string json_body = body.str();
        std::ostringstream req;
        req << "POST /v1/traces HTTP/1.1\r\nHost: " << otel_host << ":" << otel_port
            << "\r\nContent-Type: application/json\r\nContent-Length: " << json_body.size()
            << "\r\nConnection: close\r\n\r\n"
            << json_body;

        std::string request_str = req.str();
        send(sockfd, request_str.c_str(), request_str.size(), 0);
        close(sockfd);
    }).detach();
}
