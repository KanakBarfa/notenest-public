#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <notenest/event_bus.hpp>
#include <print>
#include <vector>

EventBus::EventBus() = default;

EventBus::~EventBus() {
    stopHeartbeat();
}

void EventBus::startHeartbeat(int interval_seconds) {
    if (running_.exchange(true)) {
        return;
    }
    heartbeat_thread_ = std::thread([this, interval_seconds]() {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(interval_seconds));
            if (!running_) {
                break;
            }
            heartbeat();
        }
    });
}

void EventBus::stopHeartbeat() {
    if (!running_.exchange(false)) {
        return;
    }
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
}

void EventBus::subscribe(const std::string& user_id, int fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    subscribers_[user_id].insert(fd);
    fd_to_user_[fd] = user_id;
    std::println("EventBus::subscribe - user_id: {} fd: {}", user_id, fd);
}

void EventBus::unsubscribe(const std::string& user_id, int fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscribers_.find(user_id);
    if (it != subscribers_.end()) {
        it->second.erase(fd);
        if (it->second.empty()) {
            subscribers_.erase(it);
        }
    }
    fd_to_user_.erase(fd);
    std::println("EventBus::unsubscribe - user_id: {} fd: {}", user_id, fd);
}

void EventBus::unsubscribeFd(int fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = fd_to_user_.find(fd);
    if (it != fd_to_user_.end()) {
        const std::string& user_id = it->second;
        auto sub_it = subscribers_.find(user_id);
        if (sub_it != subscribers_.end()) {
            sub_it->second.erase(fd);
            if (sub_it->second.empty()) {
                subscribers_.erase(sub_it);
            }
        }
        std::println("EventBus::unsubscribeFd - user_id: {} fd: {}", user_id, fd);
        fd_to_user_.erase(it);
    }
}

void EventBus::publish(const std::string& user_id, const std::string& event_json) {
    std::vector<int> target_fds;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = subscribers_.find(user_id);
        if (it == subscribers_.end() || it->second.empty()) {
            return;
        }
        target_fds.assign(it->second.begin(), it->second.end());
    }

    std::string raw_message = "data: " + event_json + "\n\n";
    std::string message = std::format("{:x}\r\n{}\r\n", raw_message.size(), raw_message);
    std::vector<int> dead_fds;

    for (int fd : target_fds) {
        if (!sendData(fd, message)) {
            dead_fds.push_back(fd);
        }
    }

    for (int fd : dead_fds) {
        unsubscribeFd(fd);
    }
}

void EventBus::heartbeat() {
    std::vector<int> active_fds;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [fd, user_id] : fd_to_user_) {
            active_fds.push_back(fd);
        }
    }

    std::string raw_ping = ": keep-alive\n\n";
    std::string ping = std::format("{:x}\r\n{}\r\n", raw_ping.size(), raw_ping);
    std::vector<int> dead_fds;

    for (int fd : active_fds) {
        if (!sendData(fd, ping)) {
            dead_fds.push_back(fd);
        }
    }

    for (int fd : dead_fds) {
        unsubscribeFd(fd);
    }
}

bool EventBus::sendData(int fd, const std::string& data) {
    size_t total_sent = 0;
    while (total_sent < data.size()) {
        ssize_t sent = send(fd, data.data() + total_sent, data.size() - total_sent, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd;
                pfd.fd = fd;
                pfd.events = POLLOUT;
                int ret = poll(&pfd, 1, 100);
                if (ret <= 0) {
                    return false;
                }
                continue;
            }
            return false;
        }
        total_sent += sent;
    }
    return true;
}
