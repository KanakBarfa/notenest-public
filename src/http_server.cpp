#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <notenest/http_parser.hpp>
#include <notenest/http_server.hpp>
#include <notenest/websocket.hpp>
#include <print>
#include <vector>

using namespace std::chrono_literals;

namespace {
std::atomic<uint64_t> g_next_gen{1};

std::chrono::steady_clock::time_point now() {
    return std::chrono::steady_clock::now();
}

int64_t elapsedSecs(std::chrono::steady_clock::time_point from) {
    return std::chrono::duration_cast<std::chrono::seconds>(now() - from).count();
}
}  // namespace

// BlockingPool: bounded pool executing blocking handlers off the event loops.

HttpServer::BlockingPool::BlockingPool(size_t threads) {
    threads_.reserve(threads);
    for (size_t i = 0; i < threads; ++i) {
        threads_.emplace_back([this] { workerLoop(); });
    }
}

HttpServer::BlockingPool::~BlockingPool() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stopping_ = true;
    }
    cv_.notify_all();
    // Ordered shutdown: workers exit once the queue drains.
    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
}

void HttpServer::BlockingPool::workerLoop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (tasks_.empty()) {
                if (stopping_)
                    return;
                continue;
            }
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        try {
            task();
        } catch (const std::exception& e) {
            std::println(stderr, "[Pool] Task exception: {}", e.what());
        } catch (...) {
            std::println(stderr, "[Pool] Unknown task exception");
        }
    }
}

void HttpServer::BlockingPool::submit(std::function<void()> task) {
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!stopping_) {
            tasks_.push_back(std::move(task));
            queued = true;
        }
    }
    if (queued) {
        cv_.notify_one();
    } else {
        std::println(stderr, "[Pool] Submit rejected during shutdown");
    }
}

HttpServer::HttpServer(int port, Router& router, EventBus* event_bus, RoomRegistry* room_registry,
                       unsigned worker_count)
    : port_(port),
      router_(router),
      event_bus_(event_bus),
      room_registry_(room_registry),
      blocking_(16) {
    unsigned n = worker_count ? worker_count : std::thread::hardware_concurrency();
    if (n == 0)
        n = 4;
    n = std::clamp(n, 2u, 8u);

    workers_.reserve(n);
    for (unsigned i = 0; i < n; ++i) {
        auto w = std::make_unique<Worker>();
        w->idx = i;
        workers_.push_back(std::move(w));
    }

    if (event_bus_) {
        event_bus_->setSink([this](int fd, uint64_t gen, const std::string& data) -> bool {
            return postTo(fd, gen, data, false);
        });
    }
}

HttpServer::~HttpServer() {
    stop();
}

HttpServer::Worker& HttpServer::ownerOf(int fd) {
    // Deterministic ownership shared by acceptor and all publishers.
    size_t idx = static_cast<size_t>(fd) % workers_.size();
    return *workers_[idx];
}

bool HttpServer::postTo(int fd, uint64_t gen, std::string data, bool close_after) {
    Worker& w = ownerOf(fd);
    {
        std::lock_guard<std::mutex> lock(w.mu);
        auto it = w.conns.find(fd);
        if (it == w.conns.end() || it->second.gen != gen) {
            return false;  // stale handle: fd closed or reused
        }
        w.tasks.push_back([this, &w, fd, gen, data = std::move(data), close_after]() mutable {
            applyOutbound(w, fd, gen, std::move(data), close_after);
        });
    }
    uint64_t one = 1;
    ssize_t rc = write(w.wake_efd, &one, sizeof(one));
    (void)rc;
    return true;
}

void HttpServer::start() {
    if (running_.exchange(true)) {
        return;
    }

    server_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (server_fd_ < 0) {
        std::println(stderr, "Failed to create socket, errno: {}", errno);
        running_ = false;
        return;
    }

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port_);

    if (bind(server_fd_, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) < 0 ||
        listen(server_fd_, SOMAXCONN) < 0) {
        std::println(stderr, "Failed to bind/listen port {}, errno: {}", port_, errno);
        ::close(server_fd_);
        server_fd_ = -1;
        running_ = false;
        return;
    }

    for (auto& w : workers_) {
        w->thread = std::make_unique<std::thread>([this, ptr = w.get()] { runWorker(ptr); });
    }

    std::println("Server listening on port {} with {} epoll workers", port_, workers_.size());
    runAcceptor();

    // Acceptor returned: stop() was requested. Wait for workers to drain.
    for (auto& w : workers_) {
        if (w->thread && w->thread->joinable()) {
            w->thread->join();
        }
    }
    std::println("Server stopped gracefully");
}

void HttpServer::runAcceptor() {
    std::vector<struct pollfd> pfd(1);
    pfd[0].fd = server_fd_;
    pfd[0].events = POLLIN;

    while (running_) {
        int rc = poll(pfd.data(), 1, 200);
        if (rc <= 0) {
            continue;
        }

        while (running_) {
            int cfd = accept4(server_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (cfd < 0) {
                break;  // EAGAIN or transient error; back to poll()
            }

            uint64_t gen = g_next_gen.fetch_add(1, std::memory_order_relaxed);
            // Must match ownerOf(): fd % worker_count.
            Worker& w = *workers_[static_cast<size_t>(cfd) % workers_.size()];
            {
                std::lock_guard<std::mutex> lock(w.mu);
                w.tasks.push_back([this, &w, cfd, gen] {
                    Connection c;
                    c.fd = cfd;
                    c.gen = gen;
                    c.last_activity = now();
                    c.next_keep_alive = now() + std::chrono::seconds(kSseKeepAliveSecs);
                    w.conns.emplace(cfd, std::move(c));
                    struct epoll_event ev{};
                    ev.events = EPOLLIN;
                    ev.data.fd = cfd;
                    if (epoll_ctl(w.epoll_fd, EPOLL_CTL_ADD, cfd, &ev) < 0) {
                        std::println(stderr, "epoll ADD failed for fd {}", cfd);
                        closeConn(w, cfd);
                    }
                });
            }
            uint64_t one = 1;
            ssize_t r = write(w.wake_efd, &one, sizeof(one));
            (void)r;
        }
    }
}

void HttpServer::initWorker(Worker& w) {
    w.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    w.wake_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    w.timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (w.epoll_fd < 0 || w.wake_efd < 0 || w.timer_fd < 0) {
        std::println(stderr, "[Worker {}] Failed creating epoll/efd/timer", w.idx);
        std::_Exit(1);
    }

    itimerspec ts{};
    ts.it_value.tv_sec = kTimerIntervalMs / 1000;
    ts.it_interval.tv_sec = kTimerIntervalMs / 1000;
    timerfd_settime(w.timer_fd, 0, &ts, nullptr);

    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = w.wake_efd;
    epoll_ctl(w.epoll_fd, EPOLL_CTL_ADD, w.wake_efd, &ev);
    ev.data.fd = w.timer_fd;
    epoll_ctl(w.epoll_fd, EPOLL_CTL_ADD, w.timer_fd, &ev);
}

void HttpServer::processTasks(Worker& w) {
    std::deque<std::function<void()>> batch;
    {
        std::lock_guard<std::mutex> lock(w.mu);
        batch.swap(w.tasks);
    }
    while (!batch.empty()) {
        auto task = std::move(batch.front());
        batch.pop_front();
        task();
    }
}

void HttpServer::runWorker(Worker* wp) {
    Worker& w = *wp;
    initWorker(w);

    std::vector<struct epoll_event> events(256);

    while (true) {
        int n = epoll_wait(w.epoll_fd, events.data(), static_cast<int>(events.size()),
                           running_ ? -1 : 50);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            if (fd == w.wake_efd) {
                uint64_t v;
                while (::read(w.wake_efd, &v, sizeof(v)) > 0) {
                }
                processTasks(w);
            } else if (fd == w.timer_fd) {
                uint64_t expirations = 0;
                while (::read(w.timer_fd, &expirations, sizeof(expirations)) > 0) {
                }
                onTimerTick(w);
            } else if (ev & (EPOLLERR | EPOLLHUP)) {
                closeConn(w, fd);
            } else {
                if (ev & EPOLLOUT) {
                    handleWritable(w, fd);
                }
                if (ev & EPOLLIN) {
                    handleReadable(w, fd);
                }
            }
        }

        processTasks(w);

        if (!running_) {
            bool idle;
            {
                std::lock_guard<std::mutex> lock(w.mu);
                idle = w.tasks.empty() && w.conns.empty();
            }
            if (idle) {
                break;
            }
        }
    }

    std::vector<int> remaining;
    {
        std::lock_guard<std::mutex> lock(w.mu);
        for (auto& [fd, c] : w.conns)
            remaining.push_back(fd);
    }
    for (int fd : remaining) {
        closeConn(w, fd);
    }
    ::close(w.timer_fd);
    ::close(w.wake_efd);
    ::close(w.epoll_fd);
}

// SSE keep-alives, WS liveness pings, idle sweeps. Owning worker only.
void HttpServer::onTimerTick(Worker& w) {
    auto tnow = now();
    std::vector<int> to_close;
    for (auto& [fd, c] : w.conns) {
        if ((c.is_sse || c.is_websocket) && !c.close_after_flush && tnow >= c.next_keep_alive) {
            c.next_keep_alive = tnow + std::chrono::seconds(kSseKeepAliveSecs);
            std::string keep =
                c.is_sse ? EventBus::keepAliveFrame() : WebSocket::encodeFrame("", 0x89);
            applyOutbound(w, fd, c.gen, std::move(keep), false);
            continue;
        }
        if (!c.is_sse && !c.is_websocket && !c.busy &&
            c.request_deadline != std::chrono::steady_clock::time_point{} &&
            tnow >= c.request_deadline) {
            to_close.push_back(fd);  // stalled/partial request: slowloris guard
            continue;
        }
        if (!c.is_sse && !c.is_websocket && !c.busy &&
            elapsedSecs(c.last_activity) >= kIdleTimeoutSecs) {
            to_close.push_back(fd);
        }
    }
    for (int fd : to_close) {
        closeConn(w, fd);
    }
}

void HttpServer::armOut(Worker& w, Connection& c, bool armed) {
    if (c.out_armed == armed)
        return;
    struct epoll_event ev{};
    ev.events = armed ? (EPOLLIN | EPOLLOUT) : EPOLLIN;
    ev.data.fd = c.fd;
    if (epoll_ctl(w.epoll_fd, EPOLL_CTL_MOD, c.fd, &ev) == 0) {
        c.out_armed = armed;
    }
}

// Flushes pending outbound bytes (owning worker only).
bool HttpServer::flushOut(Worker& w, Connection& c) {
    while (c.out_off < c.out_buf.size()) {
        ssize_t sent =
            ::send(c.fd, c.out_buf.data() + c.out_off, c.out_buf.size() - c.out_off, MSG_NOSIGNAL);
        if (sent >= 0) {
            c.out_off += static_cast<size_t>(sent);
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            armOut(w, c, true);
            return true;  // pending; resumes on EPOLLOUT
        }
        return false;
    }
    c.out_buf.clear();
    c.out_off = 0;
    armOut(w, c, false);
    return true;
}

void HttpServer::applyOutbound(Worker& w, int fd, uint64_t gen, std::string data,
                               bool close_after) {
    auto it = w.conns.find(fd);
    if (it == w.conns.end() || it->second.gen != gen) {
        return;
    }
    Connection& c = it->second;

    if (close_after) {
        c.close_after_flush = true;
    }

    // Slow-consumer backpressure: drop connections past the cap.
    if (!data.empty()) {
        size_t pending = c.out_buf.size() - c.out_off;
        if (pending + data.size() > kMaxOutBufBytes) {
            std::println(stderr, "[Server] Slow consumer on fd {}: output cap reached", fd);
            closeConn(w, fd);
            return;
        }
        c.out_buf.append(data);
    }

    if (!flushOut(w, c)) {
        closeConn(w, fd);
        return;
    }
    if (c.close_after_flush && c.out_off >= c.out_buf.size()) {
        closeConn(w, fd);
    }
}

void HttpServer::closeConn(Worker& w, int fd) {
    auto it = w.conns.find(fd);
    if (it == w.conns.end())
        return;

    if (room_registry_) {
        ConnId left{fd, it->second.gen};
        std::string msg;
        std::vector<ConnId> targets;
        room_registry_->unregisterFd(left, msg, targets);
        if (!msg.empty() && !targets.empty()) {
            std::string frame = WebSocket::encodeFrame(msg, 0x01);
            for (ConnId t : targets) {
                postTo(t.fd, t.gen, frame, false);
            }
        }
    }
    if (event_bus_) {
        event_bus_->unsubscribeFd(fd);
    }

    epoll_ctl(w.epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);
    w.conns.erase(it);
}

void HttpServer::handleReadable(Worker& w, int fd) {
    auto it = w.conns.find(fd);
    if (it == w.conns.end())
        return;
    Connection& c = it->second;
    c.last_activity = now();

    char buf[16384];
    while (true) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            c.read_buf.append(buf, static_cast<size_t>(n));
            if (c.read_buf.size() > kMaxReadBufBytes) {
                std::println(stderr, "[Server] Read buffer cap exceeded on fd {}", fd);
                closeConn(w, fd);
                return;
            }
            continue;
        }
        if (n == 0) {
            closeConn(w, fd);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
        closeConn(w, fd);
        return;
    }

    if (c.is_websocket) {
        processWsFrames(w, fd);
    } else {
        dispatchRequest(w, c);
    }
}

void HttpServer::handleWritable(Worker& w, int fd) {
    auto it = w.conns.find(fd);
    if (it == w.conns.end())
        return;
    Connection& c = it->second;
    c.last_activity = now();

    if (!flushOut(w, c)) {
        closeConn(w, fd);
        return;
    }
    if (c.close_after_flush && c.out_off >= c.out_buf.size()) {
        closeConn(w, fd);
    }
}

void HttpServer::dispatchRequest(Worker& w, Connection& c) {
    // Parse as many pipelined requests as available.
    while (!c.busy && !c.close_after_flush) {
        HttpRequest req;
        size_t consumed = 0;
        auto result = HttpParser::parse(c.read_buf, req, consumed);
        if (result == HttpParser::Result::Incomplete) {
            auto zero = std::chrono::steady_clock::time_point{};
            if (!c.read_buf.empty() && c.request_deadline == zero) {
                c.request_deadline = now() + std::chrono::seconds(kRequestTimeoutSecs);
            }
            return;
        }
        c.request_deadline = {};
        if (result == HttpParser::Result::Error) {
            c.read_buf.clear();
            HttpResponse bad;
            bad.status_code = 400;
            bad.status_text = "Bad Request";
            bad.body = "{\"error\":\"malformed_request\"}";
            bad.headers["Content-Type"] = "application/json";
            bad.keep_alive = false;
            c.out_buf.append(bad.toString());
            c.close_after_flush = true;
            if (!flushOut(w, c)) {
                closeConn(w, c.fd);
            } else if (c.out_off >= c.out_buf.size()) {
                closeConn(w, c.fd);
            }
            return;
        }
        c.read_buf.erase(0, consumed);
        c.busy = true;

        ConnId id{c.fd, c.gen};
        blocking_.submit([this, id, req]() mutable {
            HttpResponse res = router_.route(req);
            enqueueCompletion(id, std::move(req), std::move(res));
        });
    }
}

void HttpServer::completeResponse(Worker& w, int fd, uint64_t gen, HttpRequest req,
                                  HttpResponse res) {
    // Runs as a queued task on the owning worker.
    auto it = w.conns.find(fd);
    if (it == w.conns.end() || it->second.gen != gen) {
        return;
    }
    Connection& c = it->second;
    c.busy = false;

    bool upgraded = res.is_sse || res.is_websocket;
    if (!upgraded) {
        res.keep_alive = req.keep_alive;
    }

    // Internal routing headers must never reach the wire.
    std::string ws_note = res.headers.count("X-Note-Id") ? res.headers["X-Note-Id"] : "";
    std::string ws_email = res.headers.count("X-User-Email") ? res.headers["X-User-Email"] : "";
    bool ws_editor = res.headers.count("X-Note-Editor") && res.headers["X-Note-Editor"] == "1";
    std::erase_if(res.headers, [](const auto& kv) { return kv.first.rfind("X-", 0) == 0; });

    std::string payload = res.toString();
    if (upgraded) {
        c.is_sse = res.is_sse;
        c.is_websocket = res.is_websocket;
        c.next_keep_alive = now() + std::chrono::seconds(kSseKeepAliveSecs);
    } else {
        c.close_after_flush = !req.keep_alive;
    }
    c.out_buf.append(payload);

    if (!flushOut(w, c)) {
        closeConn(w, fd);
        return;
    }
    if (c.close_after_flush && c.out_off >= c.out_buf.size()) {
        closeConn(w, fd);
        return;
    }

    if (res.is_websocket && room_registry_) {
        if (!ws_note.empty()) {
            std::string msg;
            std::vector<ConnId> targets;
            room_registry_->joinRoom(ws_note, ConnId{fd, gen}, req.user_id, ws_email, ws_editor,
                                     msg, targets);
            if (!msg.empty() && !targets.empty()) {
                std::string frame = WebSocket::encodeFrame(msg, 0x01);
                for (ConnId t : targets) {
                    postTo(t.fd, t.gen, frame, false);
                }
            }
        }
    }
    if (res.is_sse && event_bus_) {
        // Subscribe after headers are queued to preserve frame order.
        event_bus_->subscribe(req.user_id, ConnId{fd, gen});
    }

    if (!c.read_buf.empty() && !c.busy && !c.close_after_flush) {
        dispatchRequest(w, c);
    }
}

void HttpServer::enqueueCompletion(ConnId id, HttpRequest req, HttpResponse res) {
    Worker& w = ownerOf(id.fd);
    {
        std::lock_guard<std::mutex> lock(w.mu);
        w.tasks.push_back([this, &w, id, req = std::move(req), res = std::move(res)]() mutable {
            completeResponse(w, id.fd, id.gen, std::move(req), std::move(res));
        });
    }
    uint64_t one = 1;
    ssize_t rc = write(w.wake_efd, &one, sizeof(one));
    (void)rc;
}

// Decoded WebSocket frames; broadcasts fan out via generation-checked posts.
void HttpServer::processWsFrames(Worker& w, int fd) {
    auto it = w.conns.find(fd);
    if (it == w.conns.end())
        return;
    Connection& c = it->second;

    WebSocket::Frame frame;
    while (WebSocket::parseFrame(c.read_buf, frame)) {
        c.read_buf.erase(0, frame.consumed_bytes);
        try {
            if (frame.opcode == 0x01) {  // JSON control/presence messages
                if (!room_registry_) {
                    continue;
                }
                std::string msg;
                std::vector<ConnId> targets;
                room_registry_->handleMessage(ConnId{fd, c.gen}, frame.payload, msg, targets);
                if (!msg.empty() && !targets.empty()) {
                    std::string out = WebSocket::encodeFrame(msg, 0x01);
                    for (ConnId t : targets) {
                        postTo(t.fd, t.gen, out, false);
                    }
                }
            } else if (frame.opcode == 0x02) {  // binary Yjs CRDT relay
                // Viewers must not relay document mutations.
                if (!room_registry_ || !room_registry_->canRelayBinary(fd)) {
                    continue;
                }
                auto targets = room_registry_->getRelayTargets(fd);
                if (targets.empty()) {
                    continue;
                }
                std::string out = WebSocket::encodeFrame(frame.payload, 0x02);
                for (ConnId t : targets) {
                    postTo(t.fd, t.gen, out, false);
                }
            } else if (frame.opcode == 0x08) {  // close
                closeConn(w, fd);
                return;
            } else if (frame.opcode == 0x09) {  // ping -> pong
                applyOutbound(w, fd, c.gen, WebSocket::encodeFrame(frame.payload, 0x0A), false);
            }
        } catch (const std::exception& e) {
            std::println(stderr, "[WS] Frame handling error on fd {}: {}", fd, e.what());
        } catch (...) {
            std::println(stderr, "[WS] Unknown frame handling error on fd {}", fd);
        }
        // May have closed while handling.
        if (w.conns.find(fd) == w.conns.end())
            return;
    }
}

void HttpServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    if (server_fd_ >= 0) {
        ::close(server_fd_);
        server_fd_ = -1;
    }

    // Wake all workers so they observe running_ == false and drain.
    for (auto& w : workers_) {
        uint64_t one = 1;
        ssize_t r = write(w->wake_efd, &one, sizeof(one));
        (void)r;
    }
}
