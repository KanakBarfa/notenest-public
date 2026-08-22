#ifndef HTTP_SERVER_HPP
#define HTTP_SERVER_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <notenest/conn_id.hpp>
#include <notenest/event_bus.hpp>
#include <notenest/room_registry.hpp>
#include <notenest/router.hpp>
#include <thread>
#include <unordered_map>
#include <vector>

// Per-connection state; owned exclusively by its worker thread.
struct Connection {
    int fd = -1;
    uint64_t gen = 0;
    std::string read_buf;
    std::string out_buf;
    size_t out_off = 0;  // flush cursor into out_buf
    bool is_websocket = false;
    bool is_sse = false;
    bool close_after_flush = false;
    bool busy = false;       // request in flight on the blocking pool
    bool out_armed = false;  // EPOLLOUT currently registered
    std::chrono::steady_clock::time_point last_activity{};
    std::chrono::steady_clock::time_point next_keep_alive{};
    // Deadline for receiving a complete request; zero = not armed.
    std::chrono::steady_clock::time_point request_deadline{};
};

// Acceptor + N epoll workers; cross-thread posts via generation-checked queues.
class HttpServer {
public:
    HttpServer(int port, Router& router, EventBus* event_bus = nullptr,
               RoomRegistry* room_registry = nullptr, unsigned worker_count = 0);
    ~HttpServer();

    // Blocks serving until stop().
    void start();

    // Ordered shutdown: stop accepting, drain queues, flush, close.
    void stop();

private:
    static constexpr size_t kMaxOutBufBytes = 4u * 1024u * 1024u;  // backpressure cap
    static constexpr size_t kMaxReadBufBytes = 8u * 1024u * 1024u;
    static constexpr int kTimerIntervalMs = 5000;
    static constexpr int kSseKeepAliveSecs = 15;
    static constexpr int kIdleTimeoutSecs = 180;
    static constexpr int kRequestTimeoutSecs = 20;

    struct Worker {
        unsigned idx = 0;
        int epoll_fd = -1;
        int wake_efd = -1;
        int timer_fd = -1;
        std::mutex mu;  // guards conns + tasks
        std::unordered_map<int, Connection> conns;
        std::deque<std::function<void()>> tasks;
        std::unique_ptr<std::thread> thread;
        bool ready = false;
    };

    // Bounded pool executing blocking handlers off the event loops.
    class BlockingPool {
    public:
        explicit BlockingPool(size_t threads);
        ~BlockingPool();
        void submit(std::function<void()> task);

    private:
        void workerLoop();
        std::vector<std::thread> threads_;
        std::deque<std::function<void()>> tasks_;
        std::mutex mu_;
        std::condition_variable cv_;
        bool stopping_ = false;
    };

    Worker& ownerOf(int fd);
    void initWorker(Worker& w);
    void runAcceptor();
    void runWorker(Worker* w);
    void processTasks(Worker& w);
    void onTimerTick(Worker& w);
    void handleReadable(Worker& w, int fd);
    void handleWritable(Worker& w, int fd);
    void processWsFrames(Worker& w, int fd);
    void dispatchRequest(Worker& w, Connection& c);
    void completeResponse(Worker& w, int fd, uint64_t gen, HttpRequest req, HttpResponse res);
    void enqueueCompletion(ConnId id, HttpRequest req, HttpResponse res);
    void applyOutbound(Worker& w, int fd, uint64_t gen, std::string data, bool close_after);
    void armOut(Worker& w, Connection& c, bool armed);
    bool flushOut(Worker& w, Connection& c);  // false => connection dead
    void closeConn(Worker& w, int fd);
    bool postTo(int fd, uint64_t gen, std::string data, bool close_after);

    int port_;
    int server_fd_ = -1;
    std::atomic<bool> running_{false};
    Router& router_;
    EventBus* event_bus_ = nullptr;
    RoomRegistry* room_registry_ = nullptr;
    std::vector<std::unique_ptr<Worker>> workers_;
    BlockingPool blocking_;
};

#endif
