#ifndef ROOM_REGISTRY_HPP
#define ROOM_REGISTRY_HPP

#include <cstdint>
#include <mutex>
#include <notenest/conn_id.hpp>
#include <string>
#include <unordered_map>
#include <vector>

// Represents a connected participant in a note room.
struct Participant {
    ConnId id{};
    std::string user_id;
    std::string email;
    bool editor = false;  // viewers' Yjs frames are never relayed
};

// Represents a real-time room for a note.
class Room {
public:
    explicit Room(std::string note_id);

    // Adds a participant to the room.
    void join(const ConnId& id, const std::string& user_id, const std::string& email, bool editor);

    // Removes a participant from the room.
    bool leave(int fd, std::string& out_user_id);

    // Checks if the room has no participants.
    bool empty() const;

    // Gets all active participants in the room.
    std::vector<Participant> getParticipants() const;

    // Gets target connection ids for broadcasting.
    std::vector<ConnId> getTargets(int exclude_fd = -1) const;

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

    // Joins a note room and prepares the presence broadcast.
    bool joinRoom(const std::string& note_id, ConnId id, const std::string& user_id,
                  const std::string& email, bool editor, std::string& out_broadcast_msg,
                  std::vector<ConnId>& out_targets);

    // Unregisters a connection when disconnected and prepares presence update.
    void unregisterFd(ConnId id, std::string& out_broadcast_msg, std::vector<ConnId>& out_targets);

    // Handles an incoming JSON WebSocket payload; prepares broadcast.
    void handleMessage(ConnId sender, const std::string& json_payload,
                       std::string& out_broadcast_msg, std::vector<ConnId>& out_targets);

    // Whether the sender may relay binary Yjs frames (editors/owners only).
    bool canRelayBinary(int sender_fd);

    // Relay target connections for binary frame forwarding (excludes sender).
    std::vector<ConnId> getRelayTargets(int sender_fd);

private:
    std::unordered_map<std::string, std::shared_ptr<Room>> rooms_;
    std::unordered_map<int, std::string> fd_to_note_id_;
    std::mutex registry_mutex_;
};

#endif
