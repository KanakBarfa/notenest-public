#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <nlohmann/json.hpp>
#include <notenest/consul_client.hpp>
#include <print>
#include <sstream>

using json = nlohmann::json;

ConsulClient::ConsulClient(std::string consul_host, int consul_port) {
    if (!consul_host.empty()) {
        consul_host_ = consul_host;
    } else {
        const char* env_host = std::getenv("CONSUL_HOST");
        consul_host_ = env_host ? env_host : "consul";
    }

    if (consul_port > 0) {
        consul_port_ = consul_port;
    } else {
        const char* env_port = std::getenv("CONSUL_PORT");
        consul_port_ = env_port ? std::stoi(env_port) : 8500;
    }
}

ConsulClient::~ConsulClient() {
    stopHeartbeat();
}

std::string ConsulClient::getContainerAddress() {
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        struct hostent* he = gethostbyname(hostname);
        if (he && he->h_addr_list[0]) {
            struct in_addr addr;
            std::memcpy(&addr, he->h_addr_list[0], sizeof(struct in_addr));
            std::string ip_str = inet_ntoa(addr);
            if (!ip_str.empty() && ip_str.find("127.") != 0) {
                return ip_str;
            }
        }
        return std::string(hostname);
    }
    return "127.0.0.1";
}

static std::string unchunkBody(const std::string& raw_body) {
    std::string result;
    size_t pos = 0;
    while (pos < raw_body.size()) {
        size_t line_end = raw_body.find("\r\n", pos);
        if (line_end == std::string::npos)
            break;
        std::string hex_str = raw_body.substr(pos, line_end - pos);
        size_t semi = hex_str.find(';');
        if (semi != std::string::npos)
            hex_str = hex_str.substr(0, semi);

        long chunk_size = 0;
        try {
            chunk_size = std::stol(hex_str, nullptr, 16);
        } catch (...) {
            break;
        }
        if (chunk_size <= 0)
            break;
        pos = line_end + 2;
        if (pos + chunk_size <= raw_body.size()) {
            result.append(raw_body.data() + pos, chunk_size);
            pos += chunk_size + 2;
        } else {
            result.append(raw_body.data() + pos, raw_body.size() - pos);
            break;
        }
    }
    return result.empty() ? raw_body : result;
}

static std::string extractResponseBody(const std::string& response) {
    size_t body_pos = response.find("\r\n\r\n");
    if (body_pos == std::string::npos)
        return "";

    std::string headers = response.substr(0, body_pos);
    std::string raw_body = response.substr(body_pos + 4);

    std::string headers_lower = headers;
    for (char& c : headers_lower)
        c = ::tolower(c);

    if (headers_lower.find("transfer-encoding: chunked") != std::string::npos) {
        return unchunkBody(raw_body);
    }
    return raw_body;
}

std::string ConsulClient::httpGet(const std::string& path) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return "";

    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

    struct hostent* hp = gethostbyname(consul_host_.c_str());
    if (!hp) {
        close(sock);
        return "";
    }

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(consul_port_);
    std::memcpy(&addr.sin_addr, hp->h_addr_list[0], hp->h_length);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return "";
    }

    std::ostringstream request;
    request << "GET " << path << " HTTP/1.1\r\n"
            << "Host: " << consul_host_ << "\r\n"
            << "Accept: application/json\r\n"
            << "Connection: close\r\n\r\n";

    std::string req_str = request.str();
    send(sock, req_str.c_str(), req_str.length(), 0);

    std::string response;
    char buffer[4096];
    ssize_t bytes_read = 0;
    while ((bytes_read = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytes_read] = '\0';
        response.append(buffer, bytes_read);
    }
    close(sock);

    return extractResponseBody(response);
}

std::string ConsulClient::httpPut(const std::string& path, const std::string& body) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return "";

    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

    struct hostent* hp = gethostbyname(consul_host_.c_str());
    if (!hp) {
        close(sock);
        return "";
    }

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(consul_port_);
    std::memcpy(&addr.sin_addr, hp->h_addr_list[0], hp->h_length);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return "";
    }

    std::ostringstream request;
    request << "PUT " << path << " HTTP/1.1\r\n"
            << "Host: " << consul_host_ << "\r\n"
            << "Content-Type: application/json\r\n"
            << "Content-Length: " << body.length() << "\r\n"
            << "Connection: close\r\n\r\n"
            << body;

    std::string req_str = request.str();
    send(sock, req_str.c_str(), req_str.length(), 0);

    std::string response;
    char buffer[4096];
    ssize_t bytes_read = 0;
    while ((bytes_read = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytes_read] = '\0';
        response.append(buffer, bytes_read);
    }
    close(sock);

    return extractResponseBody(response);
}

bool ConsulClient::registerService(const std::string& service_name, const std::string& service_id,
                                   const std::string& address, int port, int ttl_seconds) {
    json body = {{"ID", service_id},
                 {"Name", service_name},
                 {"Tags", {"grpc", "v0.16"}},
                 {"Address", address},
                 {"Port", port},
                 {"Check",
                  {{"CheckID", "service:" + service_id},
                   {"Name", service_name + " TTL Health Check"},
                   {"TTL", std::to_string(ttl_seconds) + "s"},
                   {"DeregisterCriticalServiceAfter", "30s"}}}};

    std::string res = httpPut("/v1/agent/service/register", body.dump());
    registered_service_ids_.push_back(service_id);
    std::println("[ConsulClient] Registered service '{}' (ID: {}) on {}:{}", service_name,
                 service_id, address, port);
    return true;
}

bool ConsulClient::sendHeartbeat(const std::string& service_id) {
    std::string res = httpPut("/v1/agent/check/pass/service:" + service_id, "");
    return true;
}

bool ConsulClient::deregisterService(const std::string& service_id) {
    std::string res = httpPut("/v1/agent/service/deregister/" + service_id, "");
    std::println("[ConsulClient] Deregistered service ID: {}", service_id);
    return true;
}

void ConsulClient::deregisterAllServices() {
    for (const auto& sid : registered_service_ids_) {
        deregisterService(sid);
    }
    registered_service_ids_.clear();
}

std::vector<std::string> ConsulClient::discoverService(const std::string& service_name) {
    std::string body = httpGet("/v1/health/service/" + service_name + "?passing=true");
    std::vector<std::string> endpoints;

    if (body.empty())
        return endpoints;

    try {
        auto parsed = json::parse(body);
        if (parsed.is_array()) {
            for (const auto& item : parsed) {
                if (item.contains("Service")) {
                    const auto& svc = item["Service"];
                    std::string addr = svc.value("Address", "");
                    if (addr.empty() && item.contains("Node")) {
                        addr = item["Node"].value("Address", "");
                    }
                    int port = svc.value("Port", 0);
                    if (!addr.empty() && port > 0) {
                        endpoints.push_back(addr + ":" + std::to_string(port));
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        std::println(stderr, "[ConsulClient] JSON parse error in discoverService: {}", e.what());
    }

    return endpoints;
}

void ConsulClient::startHeartbeat(int interval_seconds) {
    stopHeartbeat();
    heartbeat_running_ = true;

    heartbeat_thread_ = std::thread([this, interval_seconds]() {
        while (heartbeat_running_) {
            for (const auto& sid : registered_service_ids_) {
                sendHeartbeat(sid);
            }
            std::this_thread::sleep_for(std::chrono::seconds(interval_seconds));
        }
    });
}

void ConsulClient::stopHeartbeat() {
    if (heartbeat_running_) {
        heartbeat_running_ = false;
        if (heartbeat_thread_.joinable()) {
            heartbeat_thread_.join();
        }
    }
}

std::string ConsulClient::resolveGrpcTarget(const std::string& fallback_address,
                                            const std::string& service_name) {
    auto endpoints = discoverService(service_name);
    if (endpoints.empty()) {
        return fallback_address;
    }

    std::string result;
    for (size_t i = 0; i < endpoints.size(); ++i) {
        if (i > 0)
            result += ",";
        result += endpoints[i];
    }
    return result;
}
