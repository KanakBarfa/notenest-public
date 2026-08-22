#include <sys/eventfd.h>
#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <exception>
#include <memory>
#include <notenest/consul_client.hpp>
#include <notenest/crypto.hpp>
#include <notenest/db_pool.hpp>
#include <notenest/event_bus.hpp>
#include <notenest/grpc_services.hpp>
#include <notenest/http_server.hpp>
#include <notenest/kafka_producer.hpp>
#include <notenest/note_repo.hpp>
#include <notenest/note_store.hpp>
#include <notenest/object_store.hpp>
#include <notenest/outbox_relay.hpp>
#include <notenest/redis_cache.hpp>
#include <notenest/room_registry.hpp>
#include <notenest/router.hpp>
#include <notenest/user_repo.hpp>
#include <print>
#include <string_view>
#include <thread>

static std::unique_ptr<HttpServer> g_server = nullptr;
static std::unique_ptr<ConsulClient> g_consul_client = nullptr;
// Signal-safe wakeup for the shutdown monitor thread.
static int g_shutdown_efd = -1;

// Async-signal-safe: write only.
void signalHandler(int signum) {
    uint64_t one = 1;
    ssize_t rc = write(g_shutdown_efd, &one, sizeof(one));
    (void)rc;
    (void)signum;
}

void shutdownMonitor() {
    uint64_t val = 0;
    if (read(g_shutdown_efd, &val, sizeof(val)) < 0) {
        return;
    }
    std::println("\nShutdown requested, draining connections...");
    if (g_consul_client) {
        g_consul_client->stopHeartbeat();
        g_consul_client->deregisterAllServices();
    }
    if (g_server) {
        g_server->stop();
    }
}

int main() {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    g_shutdown_efd = eventfd(0, EFD_CLOEXEC);
    if (g_shutdown_efd < 0) {
        std::println(stderr, "eventfd creation failed");
        return 1;
    }
    std::thread monitor(shutdownMonitor);

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    std::signal(SIGPIPE, SIG_IGN);

    // Initialize libsodium.
    try {
        Crypto::init();
    } catch (const std::exception& e) {
        std::println(stderr, "Crypto initialization failed: {}", e.what());
        return 1;
    }

    // Initialize DB connection pools with Read/Write splitting.
    const char* db_url = std::getenv("DATABASE_URL");
    const char* db_read_url = std::getenv("DATABASE_READ_URL");
    std::string write_conn_str;
    std::string read_conn_str;

    if (db_url) {
        write_conn_str = db_url;
    } else {
        const char* db_host = std::getenv("DB_HOST");
        const char* db_port = std::getenv("DB_PORT");
        const char* db_name = std::getenv("DB_NAME");
        const char* db_user = std::getenv("DB_USER");
        const char* db_pass = std::getenv("DB_PASSWORD");

        std::string host = db_host ? db_host : "localhost";
        std::string port = db_port ? db_port : "6432";
        std::string name = db_name ? db_name : "postgres";
        std::string user = db_user ? db_user : "postgres";
        std::string pass = db_pass ? db_pass : "pass";

        write_conn_str = "host=" + host + " port=" + port + " dbname=" + name + " user=" + user +
                         " password=" + pass;
    }

    if (db_read_url) {
        read_conn_str = db_read_url;
    } else {
        const char* db_read_host = std::getenv("DB_READ_HOST");
        const char* db_read_port = std::getenv("DB_READ_PORT");
        const char* db_name = std::getenv("DB_NAME");
        const char* db_user = std::getenv("DB_USER");
        const char* db_pass = std::getenv("DB_PASSWORD");

        std::string read_host =
            db_read_host ? db_read_host
                         : (std::getenv("DB_HOST") ? std::getenv("DB_HOST") : "localhost");
        std::string read_port = db_read_port ? db_read_port : "6433";
        std::string name = db_name ? db_name : "postgres";
        std::string user = db_user ? db_user : "postgres";
        std::string pass = db_pass ? db_pass : "pass";

        read_conn_str = "host=" + read_host + " port=" + read_port + " dbname=" + name +
                        " user=" + user + " password=" + pass;
    }

    try {
        DBPool::getInstance().init(write_conn_str, read_conn_str, 10);
    } catch (const std::exception& e) {
        std::println(stderr, "Database pool initialization failed: {}", e.what());
        return 1;
    }

    const char* jwt_env = std::getenv("JWT_SECRET");
    if (!jwt_env || std::string(jwt_env).empty() ||
        jwt_env == std::string_view("default_super_secure_jwt_secret_key_12345_67890")) {
        std::println(stderr,
                     "FATAL: JWT_SECRET is missing, empty, or a known insecure default. "
                     "Set a strong random value (e.g. `make setup` or openssl rand -hex 64) "
                     "and restart.");
        return 1;
    }
    std::string jwt_secret = jwt_env;

    PgUserRepository user_repo;
    AuthService auth_service(user_repo, jwt_secret);

    const char* redis_host_env = std::getenv("REDIS_HOST");
    std::string redis_host = redis_host_env ? redis_host_env : "127.0.0.1";
    const char* redis_port_env = std::getenv("REDIS_PORT");
    int redis_port = redis_port_env ? std::stoi(redis_port_env) : 6379;

    RedisCache redis_cache(redis_host, redis_port);

    const char* minio_ep_env = std::getenv("MINIO_ENDPOINT");
    std::string minio_endpoint = minio_ep_env ? minio_ep_env : "http://127.0.0.1:9000";
    const char* minio_ak_env = std::getenv("MINIO_ACCESS_KEY");
    std::string minio_access_key = minio_ak_env ? minio_ak_env : "minioadmin";
    const char* minio_sk_env = std::getenv("MINIO_SECRET_KEY");
    std::string minio_secret_key = minio_sk_env ? minio_sk_env : "minioadmin";
    const char* minio_bk_env = std::getenv("MINIO_BUCKET");
    std::string minio_bucket = minio_bk_env ? minio_bk_env : "notenest-attachments";
    const char* minio_ext_env = std::getenv("MINIO_EXTERNAL_ENDPOINT");
    std::string minio_external_endpoint = minio_ext_env ? minio_ext_env : "http://localhost:9000";

    S3ObjectStore object_store(minio_endpoint, minio_access_key, minio_secret_key,
                               minio_external_endpoint);

    // Ensure S3 bucket is created/verified before starting Router
    bool bucket_ready = false;
    for (int i = 0; i < 10; ++i) {
        if (object_store.createBucket(minio_bucket)) {
            bucket_ready = true;
            std::println("MinIO bucket '{}' verified/created.", minio_bucket);
            break;
        }
        std::println(stderr, "Waiting for MinIO (attempt {}/10)...", i + 1);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (!bucket_ready) {
        std::println(stderr, "Warning: Failed to verify/create MinIO bucket '{}'", minio_bucket);
    }

    EventBus event_bus;

    RoomRegistry room_registry;

    PgNoteRepository note_repo;
    NoteStore store(note_repo, &redis_cache, &object_store, minio_bucket);

    const char* kafka_brokers_env = std::getenv("KAFKA_BROKERS");
    std::string kafka_brokers = kafka_brokers_env ? kafka_brokers_env : "kafka:9092";

    notenest::KafkaProducer kafka_producer(kafka_brokers);
    notenest::OutboxRelay outbox_relay(DBPool::getInstance(), kafka_producer);
    outbox_relay.start();

    // Start C++ gRPC services (NoteService on 50053, Attachment & Notification Service on 50054)
    GrpcServerRunner grpc_runner(store, user_repo, event_bus);
    grpc_runner.start();

    // Register microservices with Consul
    g_consul_client = std::make_unique<ConsulClient>();
    std::string container_addr = ConsulClient::getContainerAddress();
    std::string note_svc_id = "note-service-" + container_addr + "-50053";
    std::string att_svc_id = "attachment-service-" + container_addr + "-50054";
    g_consul_client->registerService("note-service", note_svc_id, container_addr, 50053, 10);
    g_consul_client->registerService("attachment-service", att_svc_id, container_addr, 50054, 10);
    g_consul_client->startHeartbeat(4);

    const char* auth_url_env = std::getenv("AUTH_SERVICE_URL");
    std::string auth_service_url = auth_url_env ? auth_url_env : "localhost:50051";
    AuthGrpcClient auth_grpc_client(auth_service_url);

    Router router(store, auth_service, &event_bus, &room_registry, &auth_grpc_client, &redis_cache);
    g_server = std::make_unique<HttpServer>(8080, router, &event_bus, &room_registry);

    g_server->start();

    monitor.join();
    if (g_consul_client) {
        g_consul_client->stopHeartbeat();
        g_consul_client->deregisterAllServices();
    }
    outbox_relay.stop();
    grpc_runner.stop();
    DBPool::getInstance().close();
    return 0;
}
