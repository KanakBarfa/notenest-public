#ifndef CONSUL_CLIENT_HPP
#define CONSUL_CLIENT_HPP

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// Client interface for Consul service registration, TTL health checks, and discovery.
class ConsulClient {
public:
    explicit ConsulClient(std::string consul_host = "", int consul_port = 0);
    ~ConsulClient();

    // Registers a service instance with Consul agent using a TTL health check.
    bool registerService(const std::string& service_name, const std::string& service_id,
                         const std::string& address, int port, int ttl_seconds = 10);

    // Sends a TTL heartbeat pass signal for a registered service check.
    bool sendHeartbeat(const std::string& service_id);

    // Deregisters a service instance from Consul.
    bool deregisterService(const std::string& service_id);

    // Discovers healthy instances (address:port) for a given service name.
    std::vector<std::string> discoverService(const std::string& service_name);

    // Starts a background thread to send periodic heartbeats for all registered services.
    void startHeartbeat(int interval_seconds = 4);

    // Stops the background heartbeat thread.
    void stopHeartbeat();

    // Deregisters all services registered by this client.
    void deregisterAllServices();

    // Resolves a target address or service name into a gRPC formatted endpoint string.
    std::string resolveGrpcTarget(const std::string& fallback_address,
                                  const std::string& service_name);

    // Helper function to obtain the container's IP address or hostname.
    static std::string getContainerAddress();

private:
    std::string consul_host_;
    int consul_port_;
    std::atomic<bool> heartbeat_running_{false};
    std::thread heartbeat_thread_;
    std::vector<std::string> registered_service_ids_;

    std::string httpGet(const std::string& path);
    std::string httpPut(const std::string& path, const std::string& body);
};

#endif
