#include <nlohmann/json.hpp>
#include <notenest/room_registry.hpp>

Room::Room(std::string note_id) : note_id_(std::move(note_id)) {}

void Room::join(int fd, const std::string& user_id, const std::string& email) {
    std::lock_guard<std::mutex> lock(mutex_);
    participants_[fd] = Participant{fd, user_id, email};
}

bool Room::leave(int fd, std::string& out_user_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = participants_.find(fd);
    if (it != participants_.end()) {
        out_user_id = it->second.user_id;
        participants_.erase(it);
    }
    return participants_.empty();
}

bool Room::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return participants_.empty();
}

std::vector<Participant> Room::getParticipants() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Participant> list;
    list.reserve(participants_.size());
    for (const auto& [fd, p] : participants_) {
        list.push_back(p);
    }
    return list;
}

std::vector<int> Room::getTargetFds(int exclude_fd) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<int> fds;
    for (const auto& [fd, p] : participants_) {
        if (fd != exclude_fd) {
            fds.push_back(fd);
        }
    }
    return fds;
}

Participant Room::getParticipant(int fd) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = participants_.find(fd);
    if (it != participants_.end()) {
        return it->second;
    }
    return Participant{-1, "", ""};
}

bool RoomRegistry::joinRoom(const std::string& note_id, int fd, const std::string& user_id,
                            const std::string& email, std::string& out_broadcast_msg,
                            std::vector<int>& out_target_fds) {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    auto it = rooms_.find(note_id);
    if (it == rooms_.end()) {
        it = rooms_.emplace(note_id, std::make_shared<Room>(note_id)).first;
    }
    std::shared_ptr<Room> room = it->second;
    fd_to_note_id_[fd] = note_id;
    room->join(fd, user_id, email);

    auto participants = room->getParticipants();
    nlohmann::json users_json = nlohmann::json::array();
    for (const auto& p : participants) {
        users_json.push_back({{"user_id", p.user_id}, {"email", p.email}});
    }

    nlohmann::json presence_msg = {{"type", "presence"}, {"event", "user_joined"},
                                   {"note_id", note_id}, {"user_id", user_id},
                                   {"email", email},     {"users", users_json}};

    out_broadcast_msg = presence_msg.dump();
    out_target_fds = room->getTargetFds(-1);
    return true;
}

void RoomRegistry::unregisterFd(int fd, std::string& out_broadcast_msg,
                                std::vector<int>& out_target_fds) {
    out_broadcast_msg.clear();
    out_target_fds.clear();

    std::lock_guard<std::mutex> lock(registry_mutex_);
    auto it = fd_to_note_id_.find(fd);
    if (it == fd_to_note_id_.end()) {
        return;
    }

    std::string note_id = it->second;
    fd_to_note_id_.erase(it);

    auto rit = rooms_.find(note_id);
    if (rit != rooms_.end()) {
        std::shared_ptr<Room> room = rit->second;
        std::string user_id;
        bool is_empty = room->leave(fd, user_id);

        if (is_empty) {
            rooms_.erase(rit);
        } else {
            auto participants = room->getParticipants();
            nlohmann::json users_json = nlohmann::json::array();
            for (const auto& p : participants) {
                users_json.push_back({{"user_id", p.user_id}, {"email", p.email}});
            }

            nlohmann::json presence_msg = {{"type", "presence"},
                                           {"event", "user_left"},
                                           {"note_id", note_id},
                                           {"user_id", user_id},
                                           {"users", users_json}};
            out_broadcast_msg = presence_msg.dump();
            out_target_fds = room->getTargetFds(-1);
        }
    }
}

void RoomRegistry::handleMessage(int fd, const std::string& json_payload,
                                 std::string& out_broadcast_msg, std::vector<int>& out_target_fds) {
    out_broadcast_msg.clear();
    out_target_fds.clear();

    std::shared_ptr<Room> room;
    std::string note_id;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        auto it = fd_to_note_id_.find(fd);
        if (it == fd_to_note_id_.end()) {
            return;
        }
        note_id = it->second;
        auto rit = rooms_.find(note_id);
        if (rit == rooms_.end()) {
            return;
        }
        room = rit->second;
    }

    auto j = nlohmann::json::parse(json_payload, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("type")) {
        return;
    }

    Participant sender = room->getParticipant(fd);
    std::string type = j["type"].get<std::string>();

    if (type == "presence") {
        auto participants = room->getParticipants();
        nlohmann::json users_json = nlohmann::json::array();
        for (const auto& p : participants) {
            users_json.push_back({{"user_id", p.user_id}, {"email", p.email}});
        }
        j["users"] = users_json;
        out_broadcast_msg = j.dump();
        out_target_fds = room->getTargetFds(-1);
    } else if (type == "save" || type == "saved") {
        j["type"] = "saved";
        j["sender_id"] = sender.user_id;
        j["sender_email"] = sender.email;
        out_broadcast_msg = j.dump();
        out_target_fds = room->getTargetFds(-1);
    }
}

std::vector<int> RoomRegistry::getRelayTargets(int sender_fd) {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    auto it = fd_to_note_id_.find(sender_fd);
    if (it == fd_to_note_id_.end())
        return {};
    auto rit = rooms_.find(it->second);
    if (rit == rooms_.end())
        return {};
    return rit->second->getTargetFds(sender_fd);
}
