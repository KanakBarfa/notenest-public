#include <notenest/event_bus.hpp>
#include <print>

void EventBus::setSink(Sink sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    sink_ = std::move(sink);
}

std::string EventBus::formatSseFrame(const std::string& raw) {
    return std::format("{:x}\r\n{}\r\n", raw.size(), raw);
}

const std::string& EventBus::keepAliveFrame() {
    static const std::string kFrame = formatSseFrame(": keep-alive\n\n");
    return kFrame;
}

void EventBus::subscribe(const std::string& user_id, ConnId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    subscribers_[user_id][id.fd] = id.gen;
    fd_to_user_[id.fd] = user_id;
}

void EventBus::unsubscribeFd(int fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = fd_to_user_.find(fd);
    if (it == fd_to_user_.end()) {
        return;
    }
    auto sub_it = subscribers_.find(it->second);
    if (sub_it != subscribers_.end()) {
        sub_it->second.erase(fd);
        if (sub_it->second.empty()) {
            subscribers_.erase(sub_it);
        }
    }
    fd_to_user_.erase(it);
}

void EventBus::publish(const std::string& user_id, const std::string& event_json) {
    Sink sink;
    std::vector<std::pair<int, uint64_t>> targets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = subscribers_.find(user_id);
        if (it == subscribers_.end() || it->second.empty()) {
            return;
        }
        targets.reserve(it->second.size());
        for (const auto& [fd, gen] : it->second) {
            targets.emplace_back(fd, gen);
        }
        sink = sink_;
    }

    // One chunk per message keeps frames from interleaving.
    const std::string frame = formatSseFrame("data: " + event_json + "\n\n");

    if (!sink) {
        return;  // server not wired yet
    }

    std::vector<int> dead_fds;
    for (auto [fd, gen] : targets) {
        if (!sink(fd, gen, frame)) {
            dead_fds.push_back(fd);
        }
    }
    for (int fd : dead_fds) {
        unsubscribeFd(fd);
    }
}
