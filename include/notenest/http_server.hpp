#ifndef HTTP_SERVER_HPP
#define HTTP_SERVER_HPP

#include <notenest/event_bus.hpp>
#include <notenest/room_registry.hpp>
#include <notenest/router.hpp>
#include <string>
#include <unordered_map>

// Struct representing an active client connection.
struct Connection {
    int fd;
    std::string read_buf;
    bool is_websocket = false;
};

// HTTP Server using Linux epoll for non-blocking I/O.
class HttpServer {
public:
    HttpServer(int port, Router& router, EventBus* event_bus = nullptr,
               RoomRegistry* room_registry = nullptr);
    ~HttpServer();

    // Starts the server and begins listening for requests.
    void start();

    // Stops the server event loop.
    void stop();

private:
    int port_;
    int server_fd_ = -1;
    int epoll_fd_ = -1;
    bool running_ = false;
    Router& router_;
    EventBus* event_bus_ = nullptr;
    RoomRegistry* room_registry_ = nullptr;
    std::unordered_map<int, Connection> connections_;

    void closeConnection(int fd);
    void handleRead(int fd);
    bool sendAll(int fd, const std::string& data);
};

#endif
