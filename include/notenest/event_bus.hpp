#ifndef EVENT_BUS_HPP
#define EVENT_BUS_HPP

#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>

// Event bus managing SSE subscriber connections and broadcasting events.
class EventBus {
public:
    EventBus();
    ~EventBus();

    // Subscribes a client socket fd for a user ID.
    void subscribe(const std::string& user_id, int fd);

    // Unsubscribes a client socket fd for a user ID.
    void unsubscribe(const std::string& user_id, int fd);

    // Unsubscribes a socket fd from all user IDs.
    void unsubscribeFd(int fd);

    // Publishes a JSON event string to a specific user.
    void publish(const std::string& user_id, const std::string& event_json);

    // Sends heartbeat keep-alive comment to all connected SSE clients.
    void heartbeat();

    // Starts background heartbeat thread.
    void startHeartbeat(int interval_seconds = 15);

    // Stops background heartbeat thread.
    void stopHeartbeat();

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::set<int>> subscribers_;
    std::unordered_map<int, std::string> fd_to_user_;

    std::atomic<bool> running_{false};
    std::thread heartbeat_thread_;

    bool sendData(int fd, const std::string& data);
};

#endif
