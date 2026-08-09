#ifndef ROOM_REGISTRY_HPP
#define ROOM_REGISTRY_HPP

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Represents a connected participant in a note room.
struct Participant {
    int fd = -1;
    std::string user_id;
    std::string email;
};

// Represents a real-time room for a note.
class Room {
public:
    explicit Room(std::string note_id);

    // Adds a participant to the room.
    void join(int fd, const std::string& user_id, const std::string& email);

    // Removes a participant from the room.
    bool leave(int fd, std::string& out_user_id);

    // Checks if the room has no participants.
    bool empty() const;

    // Gets all active participants in the room.
    std::vector<Participant> getParticipants() const;

    // Gets target file descriptors for broadcasting.
    std::vector<int> getTargetFds(int exclude_fd = -1) const;

    // Gets participant details by socket fd.
    Participant getParticipant(int fd) const;

private:
    std::string note_id_;
    std::unordered_map<int, Participant> participants_;
    mutable std::mutex mutex_;
};

// Registry managing real-time WebSocket rooms.
class RoomRegistry {
public:
    RoomRegistry() = default;

    // Joins a note room and prepares room presence notification.
    bool joinRoom(const std::string& note_id, int fd, const std::string& user_id,
                  const std::string& email, std::string& out_broadcast_msg,
                  std::vector<int>& out_target_fds);

    // Unregisters socket fd when disconnected and prepares presence update.
    void unregisterFd(int fd, std::string& out_broadcast_msg, std::vector<int>& out_target_fds);

    // Handles incoming JSON WebSocket payload and determines broadcast target.
    void handleMessage(int fd, const std::string& json_payload, std::string& out_broadcast_msg,
                       std::vector<int>& out_target_fds);

    // Gets relay target fds for binary frame forwarding.
    std::vector<int> getRelayTargets(int sender_fd);

private:
    std::unordered_map<std::string, std::shared_ptr<Room>> rooms_;
    std::unordered_map<int, std::string> fd_to_note_id_;
    std::mutex registry_mutex_;
};

#endif
