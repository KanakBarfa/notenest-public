#ifndef EVENT_BUS_HPP
#define EVENT_BUS_HPP

#include <functional>
#include <mutex>
#include <notenest/conn_id.hpp>
#include <string>
#include <unordered_map>

// SSE subscriber registry; delivery goes through a generation-checked sink.
class EventBus {
public:
    using Sink = std::function<bool(int fd, uint64_t gen, const std::string& data)>;

    // Installs the delivery sink (wired by HttpServer at construction).
    void setSink(Sink sink);

    // Subscribes a client connection for a user ID.
    void subscribe(const std::string& user_id, ConnId id);

    // Unsubscribes a socket fd from all user IDs.
    void unsubscribeFd(int fd);

    // Publishes a JSON event string to a specific user.
    void publish(const std::string& user_id, const std::string& event_json);

    // Formats a raw SSE payload as an HTTP chunked frame.
    static std::string formatSseFrame(const std::string& raw);

    // Preformatted keep-alive comment chunk for timerfd-driven heartbeats.
    static const std::string& keepAliveFrame();

private:
    mutable std::mutex mutex_;
    // user_id -> { fd -> generation }
    std::unordered_map<std::string, std::unordered_map<int, uint64_t>> subscribers_;
    std::unordered_map<int, std::string> fd_to_user_;
    Sink sink_;
};

#endif
