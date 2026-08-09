#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <notenest/http_parser.hpp>
#include <notenest/http_server.hpp>
#include <notenest/websocket.hpp>
#include <print>
#include <vector>

constexpr int MAX_EVENTS = 64;

HttpServer::HttpServer(int port, Router& router, EventBus* event_bus, RoomRegistry* room_registry)
    : port_(port), router_(router), event_bus_(event_bus), room_registry_(room_registry) {}

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::start() {
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        std::println(stderr, "Failed to create socket, errno: {}", errno);
        return;
    }

    int opt = 1;
    if (setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::println(stderr, "Failed to set socket options");
        return;
    }

    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port_);

    if (bind(server_fd_, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::println(stderr, "Failed to bind socket to port {}, errno: {}", port_, errno);
        return;
    }

    if (listen(server_fd_, SOMAXCONN) < 0) {
        std::println(stderr, "Failed to listen, errno: {}", errno);
        return;
    }

    int flags = fcntl(server_fd_, F_GETFL, 0);
    if (flags < 0 || fcntl(server_fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        std::println(stderr, "Failed to set non-blocking on server socket");
        return;
    }

    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ < 0) {
        std::println(stderr, "Failed to create epoll, errno: {}", errno);
        return;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = server_fd_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, server_fd_, &ev) < 0) {
        std::println(stderr, "Failed to register server socket with epoll");
        return;
    }

    running_ = true;
    std::println("Server listening on port {} using epoll loop", port_);

    std::vector<struct epoll_event> events(MAX_EVENTS);

    while (running_) {
        int nfds = epoll_wait(epoll_fd_, events.data(), MAX_EVENTS, 100);
        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::println(stderr, "epoll_wait error, errno: {}", errno);
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;

            if (fd == server_fd_) {
                while (true) {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &client_len);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        std::println(stderr, "Accept error, errno: {}", errno);
                        break;
                    }

                    int cflags = fcntl(client_fd, F_GETFL, 0);
                    if (cflags < 0 || fcntl(client_fd, F_SETFL, cflags | O_NONBLOCK) < 0) {
                        std::println(stderr, "Failed to set non-blocking on client socket");
                        close(client_fd);
                        continue;
                    }

                    struct epoll_event cev;
                    cev.events = EPOLLIN;
                    cev.data.fd = client_fd;
                    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &cev) < 0) {
                        std::println(stderr, "Failed to add client socket to epoll");
                        close(client_fd);
                        continue;
                    }

                    connections_[client_fd] = Connection{client_fd, "", false};
                }
            } else {
                if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                    closeConnection(fd);
                } else if (events[i].events & EPOLLIN) {
                    handleRead(fd);
                }
            }
        }
    }
}

void HttpServer::stop() {
    if (!running_) {
        return;
    }
    running_ = false;

    // Close listening server socket first to stop accepting new connection requests
    if (server_fd_ >= 0) {
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, server_fd_, nullptr);
        close(server_fd_);
        server_fd_ = -1;
    }

    // Drain active in-flight requests gracefully up to 1 second
    std::println("[HttpServer] Connection draining active connections...");
    struct epoll_event events[MAX_EVENTS];
    auto start_drain = std::chrono::steady_clock::now();
    while (!connections_.empty()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start_drain)
                           .count();
        if (elapsed > 1000) {
            break;
        }
        int nfds = epoll_wait(epoll_fd_, events, MAX_EVENTS, 50);
        if (nfds > 0) {
            for (int i = 0; i < nfds; ++i) {
                int fd = events[i].data.fd;
                if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                    closeConnection(fd);
                } else if (events[i].events & EPOLLIN) {
                    handleRead(fd);
                }
            }
        }
    }

    if (epoll_fd_ >= 0) {
        close(epoll_fd_);
        epoll_fd_ = -1;
    }

    for (const auto& [fd, conn] : connections_) {
        close(fd);
    }
    connections_.clear();
    std::println("Server stopped gracefully");
}

void HttpServer::closeConnection(int fd) {
    if (room_registry_) {
        std::string broadcast_msg;
        std::vector<int> target_fds;
        room_registry_->unregisterFd(fd, broadcast_msg, target_fds);
        if (!broadcast_msg.empty() && !target_fds.empty()) {
            std::string frame_str = WebSocket::encodeFrame(broadcast_msg, 0x01);
            for (int target_fd : target_fds) {
                sendAll(target_fd, frame_str);
            }
        }
    }
    if (event_bus_) {
        event_bus_->unsubscribeFd(fd);
    }
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
    connections_.erase(fd);
}

void HttpServer::handleRead(int fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) {
        return;
    }

    char buf[4096];
    bool close_conn = false;
    std::string& read_buf = it->second.read_buf;

    while (true) {
        ssize_t bytes_read = recv(fd, buf, sizeof(buf), 0);
        if (bytes_read < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            close_conn = true;
            break;
        } else if (bytes_read == 0) {
            close_conn = true;
            break;
        }
        read_buf.append(buf, bytes_read);
    }

    if (close_conn) {
        closeConnection(fd);
        return;
    }

    if (it->second.is_websocket) {
        WebSocket::Frame frame;
        while (WebSocket::parseFrame(read_buf, frame)) {
            read_buf.erase(0, frame.consumed_bytes);
            if (frame.opcode == 0x01) {
                if (room_registry_) {
                    std::string broadcast_msg;
                    std::vector<int> target_fds;
                    room_registry_->handleMessage(fd, frame.payload, broadcast_msg, target_fds);
                    if (!broadcast_msg.empty() && !target_fds.empty()) {
                        std::string frame_str = WebSocket::encodeFrame(broadcast_msg, 0x01);
                        for (int target_fd : target_fds) {
                            sendAll(target_fd, frame_str);
                        }
                    }
                }
            } else if (frame.opcode == 0x02) {
                // Relay binary Yjs CRDT frames to other room participants
                if (room_registry_) {
                    auto target_fds = room_registry_->getRelayTargets(fd);
                    if (!target_fds.empty()) {
                        std::string bin_frame = WebSocket::encodeFrame(frame.payload, 0x02);
                        for (int target_fd : target_fds) {
                            sendAll(target_fd, bin_frame);
                        }
                    }
                }
            } else if (frame.opcode == 0x08) {
                closeConnection(fd);
                return;
            } else if (frame.opcode == 0x09) {
                std::string pong = WebSocket::encodeFrame(frame.payload, 0x0A);
                sendAll(fd, pong);
            }
        }
        return;
    }

    HttpRequest req;
    size_t bytes_consumed = 0;
    if (HttpParser::parse(read_buf, req, bytes_consumed)) {
        HttpResponse res = router_.route(req, fd);
        std::string res_str = res.toString();

        sendAll(fd, res_str);
        read_buf.erase(0, bytes_consumed);
        if (res.is_websocket) {
            it->second.is_websocket = true;
            if (room_registry_) {
                std::string note_id_str = res.headers["X-Note-Id"];
                std::string user_email = res.headers["X-User-Email"];
                if (!note_id_str.empty()) {
                    std::string broadcast_msg;
                    std::vector<int> target_fds;
                    room_registry_->joinRoom(note_id_str, fd, req.user_id, user_email,
                                             broadcast_msg, target_fds);
                    if (!broadcast_msg.empty() && !target_fds.empty()) {
                        std::string frame_str = WebSocket::encodeFrame(broadcast_msg, 0x01);
                        for (int target_fd : target_fds) {
                            sendAll(target_fd, frame_str);
                        }
                    }
                }
            }
        } else if (!res.is_sse) {
            closeConnection(fd);
        }
    }
}

bool HttpServer::sendAll(int fd, const std::string& data) {
    size_t total_sent = 0;
    while (total_sent < data.size()) {
        ssize_t sent = send(fd, data.data() + total_sent, data.size() - total_sent, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd;
                pfd.fd = fd;
                pfd.events = POLLOUT;
                int ret = poll(&pfd, 1, 5000);
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
