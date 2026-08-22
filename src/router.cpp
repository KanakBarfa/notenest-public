#include <sys/socket.h>

#include <chrono>
#include <nlohmann/json.hpp>
#include <notenest/crypto.hpp>
#include <notenest/observability.hpp>
#include <notenest/rabbitmq_client.hpp>
#include <notenest/redis_cache.hpp>
#include <notenest/router.hpp>
#include <notenest/utils.hpp>
#include <notenest/websocket.hpp>
#include <print>

using json = nlohmann::json;

Router::Router(NoteStore& store, AuthService& auth_service, EventBus* event_bus,
               RoomRegistry* room_registry, AuthGrpcClient* auth_grpc_client, Cache* cache)
    : store_(store),
      auth_service_(auth_service),
      auth_grpc_client_(auth_grpc_client),
      auth_middleware_(auth_service, cache),
      cache_(cache),
      event_bus_(event_bus),
      room_registry_(room_registry) {}

namespace {

// Extracts a named parameter from the request query string.
std::string extractQueryParam(const std::string& path, const std::string& name) {
    size_t qpos = path.find('?');
    if (qpos == std::string::npos)
        return "";
    std::string query = path.substr(qpos + 1);
    std::string prefix = name + "=";
    size_t start = 0;
    while (start <= query.size()) {
        size_t amp = query.find('&', start);
        std::string pair =
            query.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
        if (pair.rfind(prefix, 0) == 0) {
            return pair.substr(prefix.size());
        }
        if (amp == std::string::npos)
            break;
        start = amp + 1;
    }
    return "";
}

}  // namespace

// Helper to create a JSON error response.
static HttpResponse makeErrorResponse(int status, const std::string& status_text,
                                      const std::string& message) {
    HttpResponse res;
    res.status_code = status;
    res.status_text = status_text;
    res.headers["Content-Type"] = "application/json";
    res.body = json{{"error", message}}.dump();
    return res;
}

HttpResponse Router::route(HttpRequest& req) const {
    if (req.method == HttpMethod::OPTIONS) {
        HttpResponse res;
        res.status_code = 204;
        res.status_text = "No Content";
        res.headers["Access-Control-Allow-Origin"] = "*";
        res.headers["Access-Control-Allow-Methods"] = "GET, POST, PUT, DELETE, OPTIONS";
        res.headers["Access-Control-Allow-Headers"] = "Content-Type, Authorization";
        res.headers["Access-Control-Max-Age"] = "86400";
        return res;
    }

    auto start_time = std::chrono::steady_clock::now();
    uint64_t start_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();

    std::string tp = req.headers.count("traceparent")  ? req.headers.at("traceparent")
                     : req.headers.count("x-trace-id") ? req.headers.at("x-trace-id")
                                                       : "";
    auto [trace_id, parent_id] = Observability::parseTraceparent(tp);
    std::string span_id = Observability::generateSpanId();

    HttpResponse res;
    try {
        res = routeInternal(req);
    } catch (const std::exception& e) {
        std::println(stderr, "[Router] Handler exception: {}", e.what());
        res = makeErrorResponse(503, "Service Unavailable", e.what());
    } catch (...) {
        std::println(stderr, "[Router] Handler unknown exception");
        res = makeErrorResponse(500, "Internal Server Error", "An internal error occurred");
    }

    res.headers["Access-Control-Allow-Origin"] = "*";
    res.headers["Access-Control-Allow-Methods"] = "GET, POST, PUT, DELETE, OPTIONS";
    res.headers["Access-Control-Allow-Headers"] = "Content-Type, Authorization";
    res.headers["traceparent"] = Observability::formatTraceparent(trace_id, span_id);

    auto end_time = std::chrono::steady_clock::now();
    uint64_t end_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    double duration = std::chrono::duration<double>(end_time - start_time).count();

    std::string method_str = "GET";
    if (req.method == HttpMethod::POST)
        method_str = "POST";
    else if (req.method == HttpMethod::PUT)
        method_str = "PUT";
    else if (req.method == HttpMethod::DELETE)
        method_str = "DELETE";

    // Keep credentials/tickets out of logs and metric labels.
    std::string metrics_path = req.path.substr(0, req.path.find('?'));
    Observability::getInstance().incRequest(method_str, metrics_path, res.status_code);
    Observability::getInstance().observeDuration(method_str, duration);
    Observability::logJson(
        "INFO", method_str + " " + metrics_path + " HTTP " + std::to_string(res.status_code),
        trace_id, span_id);
    Observability::sendOtlpSpan(method_str + " " + metrics_path, trace_id, span_id, parent_id,
                                start_ns, end_ns);

    return res;
}

HttpResponse Router::routeInternal(HttpRequest& req) const {
    if (req.method == HttpMethod::UNKNOWN) {
        return makeErrorResponse(400, "Bad Request", "Unknown HTTP method");
    }

    std::string clean_path = req.path;
    size_t qpos = clean_path.find('?');
    if (qpos != std::string::npos) {
        clean_path = clean_path.substr(0, qpos);
    }
    if (clean_path.rfind("/api", 0) == 0) {
        clean_path = clean_path.substr(4);
    }

    if (clean_path == "/metrics" || clean_path == "/metrics/") {
        HttpResponse res;
        res.status_code = 200;
        res.status_text = "OK";
        res.headers["Content-Type"] = "text/plain; version=0.0.4";
        res.body = Observability::getInstance().renderMetrics();
        return res;
    }

    // Public routes: /signup and /login
    if (clean_path == "/signup" || clean_path == "/signup/") {
        if (req.method == HttpMethod::POST) {
            return handleSignup(req.body);
        } else {
            HttpResponse res =
                makeErrorResponse(405, "Method Not Allowed", "Method not allowed on /signup");
            res.headers["Allow"] = "POST";
            return res;
        }
    }

    if (clean_path == "/login" || clean_path == "/login/") {
        if (req.method == HttpMethod::POST) {
            return handleLogin(req.body);
        } else {
            HttpResponse res =
                makeErrorResponse(405, "Method Not Allowed", "Method not allowed on /login");
            res.headers["Allow"] = "POST";
            return res;
        }
    }

    if (clean_path == "/logout" || clean_path == "/logout/") {
        if (req.method == HttpMethod::POST) {
            return handleLogout(req);
        } else {
            HttpResponse res =
                makeErrorResponse(405, "Method Not Allowed", "Method not allowed on /logout");
            res.headers["Allow"] = "POST";
            return res;
        }
    }

    // Realtime endpoints also accept one-time tickets.
    bool realtime_path =
        clean_path == "/events" || clean_path == "/events/" ||
        (clean_path.size() >= 4 && clean_path.compare(clean_path.size() - 3, 3, "/ws") == 0);

    // Protected routes require authentication
    auto user_id_opt = auth_middleware_.authenticate(req);
    if (!user_id_opt && realtime_path) {
        std::string ticket = extractQueryParam(req.path, "ticket");
        if (!ticket.empty() && cache_) {
            auto redeemed = cache_->getdel("rt:ticket:" + ticket);
            if (redeemed && !redeemed->empty()) {
                user_id_opt = *redeemed;
            }
        }
    }
    if (!user_id_opt) {
        return makeErrorResponse(401, "Unauthorized", "Unauthorized");
    }
    req.user_id = *user_id_opt;

    // One-time ticket for SSE/WS; Bearer auth only.
    if (clean_path == "/realtime/ticket" || clean_path == "/realtime/ticket/") {
        if (req.method == HttpMethod::POST) {
            return handleRealtimeTicket(req.user_id);
        } else {
            HttpResponse res = makeErrorResponse(405, "Method Not Allowed",
                                                 "Method not allowed on /realtime/ticket");
            res.headers["Allow"] = "POST";
            return res;
        }
    }

    if (clean_path == "/events" || clean_path == "/events/") {
        if (req.method == HttpMethod::GET) {
            return handleEvents(req);
        } else {
            HttpResponse res =
                makeErrorResponse(405, "Method Not Allowed", "Method not allowed on /events");
            res.headers["Allow"] = "GET";
            return res;
        }
    }

    if (clean_path == "/notes" || clean_path == "/notes/") {
        if (req.method == HttpMethod::GET) {
            return handleGetNotes(req.user_id);
        } else if (req.method == HttpMethod::POST) {
            return handlePostNotes(req.body, req.user_id);
        } else {
            HttpResponse res =
                makeErrorResponse(405, "Method Not Allowed", "Method not allowed on /notes");
            res.headers["Allow"] = "GET, POST";
            return res;
        }
    }

    if (clean_path.rfind("/notes/", 0) == 0) {
        std::string sub = clean_path.substr(7);
        size_t first_slash = sub.find('/');
        if (first_slash == std::string::npos) {
            std::string id_str = sub;
            if (id_str.empty()) {
                return makeErrorResponse(404, "Not Found", "Resource not found");
            }
            if (req.method == HttpMethod::GET) {
                return handleGetNote(id_str, req.user_id);
            } else if (req.method == HttpMethod::PUT) {
                return handlePutNote(id_str, req.body, req.user_id);
            } else if (req.method == HttpMethod::DELETE) {
                return handleDeleteNote(id_str, req.user_id);
            } else {
                HttpResponse res = makeErrorResponse(405, "Method Not Allowed",
                                                     "Method not allowed on /notes/:id");
                res.headers["Allow"] = "GET, PUT, DELETE";
                return res;
            }
        } else {
            std::string id_str = sub.substr(0, first_slash);
            std::string suffix = sub.substr(first_slash);
            if (suffix == "/share" || suffix == "/share/" || suffix == "/shares" ||
                suffix == "/shares/") {
                if (req.method == HttpMethod::GET) {
                    return handleGetNoteShares(id_str, req.user_id);
                } else if (req.method == HttpMethod::POST) {
                    return handleShareNote(id_str, req.body, req.user_id);
                } else {
                    HttpResponse res =
                        makeErrorResponse(405, "Method Not Allowed", "Method not allowed");
                    res.headers["Allow"] = "GET, POST";
                    return res;
                }
            } else if (suffix.rfind("/share/", 0) == 0 || suffix.rfind("/shares/", 0) == 0) {
                size_t prefix_len = (suffix.rfind("/shares/", 0) == 0) ? 8 : 7;
                std::string target_user_id = suffix.substr(prefix_len);
                if (!target_user_id.empty() && target_user_id.back() == '/') {
                    target_user_id.pop_back();
                }
                if (req.method == HttpMethod::DELETE) {
                    return handleDeleteNoteShare(id_str, target_user_id, req.user_id);
                } else {
                    HttpResponse res =
                        makeErrorResponse(405, "Method Not Allowed", "Method not allowed");
                    res.headers["Allow"] = "DELETE";
                    return res;
                }
            } else if (suffix == "/export-pdf" || suffix == "/export-pdf/") {
                if (req.method == HttpMethod::POST) {
                    return handleExportPdf(id_str, req.user_id);
                } else {
                    HttpResponse res =
                        makeErrorResponse(405, "Method Not Allowed", "Method not allowed");
                    res.headers["Allow"] = "POST";
                    return res;
                }
            } else if (suffix == "/attachments" || suffix == "/attachments/") {
                if (req.method == HttpMethod::POST) {
                    return handlePostAttachment(id_str, req.body, req.user_id);
                } else {
                    HttpResponse res =
                        makeErrorResponse(405, "Method Not Allowed", "Method not allowed");
                    res.headers["Allow"] = "POST";
                    return res;
                }
            } else if (suffix == "/attachments/complete" || suffix == "/attachments/complete/") {
                if (req.method == HttpMethod::POST) {
                    return handleCompleteAttachment(id_str, req.body, req.user_id);
                } else {
                    HttpResponse res =
                        makeErrorResponse(405, "Method Not Allowed", "Method not allowed");
                    res.headers["Allow"] = "POST";
                    return res;
                }
            } else if (suffix.rfind("/attachments/", 0) == 0) {
                std::string att_id = suffix.substr(13);
                if (!att_id.empty() && att_id.back() == '/') {
                    att_id.pop_back();
                }
                if (req.method == HttpMethod::DELETE) {
                    return handleDeleteAttachment(id_str, att_id, req.user_id);
                } else {
                    HttpResponse res =
                        makeErrorResponse(405, "Method Not Allowed", "Method not allowed");
                    res.headers["Allow"] = "DELETE";
                    return res;
                }
            } else if (suffix == "/ws" || suffix == "/ws/") {
                if (req.method == HttpMethod::GET) {
                    return handleNoteWebSocket(id_str, req);
                } else {
                    HttpResponse res =
                        makeErrorResponse(405, "Method Not Allowed", "Method not allowed on /ws");
                    res.headers["Allow"] = "GET";
                    return res;
                }
            }
        }
    }

    return makeErrorResponse(404, "Not Found", "Resource not found");
}

HttpResponse Router::handleSignup(const std::string& body) const {
    auto j = json::parse(body, nullptr, false);
    if (j.is_discarded()) {
        return makeErrorResponse(400, "Bad Request", "Invalid JSON format");
    }

    if (!j.is_object() || !j.contains("email") || !j.contains("password") ||
        !j["email"].is_string() || !j["password"].is_string()) {
        return makeErrorResponse(400, "Bad Request",
                                 "Missing or invalid fields: email and password must be strings");
    }

    std::string email = j["email"].get<std::string>();
    std::string password = j["password"].get<std::string>();

    std::string user_id;
    if (auth_grpc_client_) {
        auto res_grpc = auth_grpc_client_->signup(email, password);
        if (res_grpc.success && !res_grpc.user_id.empty()) {
            user_id = res_grpc.user_id;
        }
    }

    if (user_id.empty()) {
        auto user_id_opt = auth_service_.signup(email, password);
        if (!user_id_opt) {
            return makeErrorResponse(400, "Bad Request", "Email already exists or invalid signup");
        }
        user_id = *user_id_opt;
    }

    HttpResponse res;
    res.status_code = 201;
    res.status_text = "Created";
    res.headers["Content-Type"] = "application/json";
    res.body = json{{"user_id", user_id}}.dump();
    return res;
}

HttpResponse Router::handleLogin(const std::string& body) const {
    auto j = json::parse(body, nullptr, false);
    if (j.is_discarded()) {
        return makeErrorResponse(400, "Bad Request", "Invalid JSON format");
    }

    if (!j.is_object() || !j.contains("email") || !j.contains("password") ||
        !j["email"].is_string() || !j["password"].is_string()) {
        return makeErrorResponse(400, "Bad Request",
                                 "Missing or invalid fields: email and password must be strings");
    }

    std::string email = j["email"].get<std::string>();
    std::string password = j["password"].get<std::string>();

    int expiry_seconds = 3600;
    if (j.contains("expiry_seconds") && j["expiry_seconds"].is_number_integer()) {
        expiry_seconds = j["expiry_seconds"].get<int>();
    }

    std::string token;
    if (auth_grpc_client_) {
        auto res_grpc = auth_grpc_client_->login(email, password, expiry_seconds);
        if (res_grpc.success && !res_grpc.token.empty()) {
            token = res_grpc.token;
        }
    }

    if (token.empty()) {
        auto token_opt = auth_service_.login(email, password, expiry_seconds);
        if (!token_opt) {
            return makeErrorResponse(401, "Unauthorized", "Invalid email or password");
        }
        token = *token_opt;
    }

    HttpResponse res;
    res.status_code = 200;
    res.status_text = "OK";
    res.headers["Content-Type"] = "application/json";
    res.body = json{{"token", token}}.dump();
    return res;
}

HttpResponse Router::handleGetNotes(const std::string& owner_id) const {
    auto notes = store_.getAllNotes(owner_id);
    HttpResponse res;
    res.status_code = 200;
    res.status_text = "OK";
    res.headers["Content-Type"] = "application/json";
    res.body = json(notes).dump();
    return res;
}

HttpResponse Router::handlePostNotes(const std::string& body, const std::string& owner_id) const {
    auto j = json::parse(body, nullptr, false);
    if (j.is_discarded()) {
        return makeErrorResponse(400, "Bad Request", "Invalid JSON format");
    }

    if (!j.is_object() || !j.contains("title") || !j.contains("content") ||
        !j["title"].is_string() || !j["content"].is_string()) {
        return makeErrorResponse(400, "Bad Request",
                                 "Missing or invalid fields: title and content must be strings");
    }

    std::string title = j["title"].get<std::string>();
    std::string content = j["content"].get<std::string>();

    if (Utils::countWords(content) > 100000) {
        return makeErrorResponse(400, "Bad Request",
                                 "Note content exceeds maximum limit of 100,000 words per note");
    }

    auto note = store_.createNote(title, content, owner_id);
    if (note.id.empty()) {
        return makeErrorResponse(500, "Internal Server Error",
                                 "Failed to create note (account may have been deleted)");
    }

    HttpResponse res;
    res.status_code = 201;
    res.status_text = "Created";
    res.headers["Content-Type"] = "application/json";
    res.body = json(note).dump();
    return res;
}

HttpResponse Router::handleGetNote(const std::string& id_str, const std::string& owner_id) const {
    auto id_opt = Utils::parseUUID(id_str);
    if (!id_opt) {
        return makeErrorResponse(400, "Bad Request", "Invalid note ID format");
    }

    auto note_opt = store_.getNoteById(*id_opt, owner_id);
    if (!note_opt) {
        return makeErrorResponse(404, "Not Found", "Note not found");
    }

    HttpResponse res;
    res.status_code = 200;
    res.status_text = "OK";
    res.headers["Content-Type"] = "application/json";
    res.body = json(*note_opt).dump();
    return res;
}

HttpResponse Router::handlePutNote(const std::string& id_str, const std::string& body,
                                   const std::string& owner_id) const {
    auto id_opt = Utils::parseUUID(id_str);
    if (!id_opt) {
        return makeErrorResponse(400, "Bad Request", "Invalid note ID format");
    }

    auto j = json::parse(body, nullptr, false);
    if (j.is_discarded()) {
        return makeErrorResponse(400, "Bad Request", "Invalid JSON format");
    }

    if (!j.is_object() || !j.contains("title") || !j.contains("content") ||
        !j["title"].is_string() || !j["content"].is_string()) {
        return makeErrorResponse(400, "Bad Request",
                                 "Missing or invalid fields: title and content must be strings");
    }

    std::string title = j["title"].get<std::string>();
    std::string content = j["content"].get<std::string>();

    if (Utils::countWords(content) > 100000) {
        return makeErrorResponse(400, "Bad Request",
                                 "Note content exceeds maximum limit of 100,000 words per note");
    }

    auto note_opt = store_.updateNote(*id_opt, title, content, owner_id);
    if (!note_opt) {
        std::string perm = store_.getUserPermissionForNote(*id_opt, owner_id);
        if (perm == "viewer") {
            return makeErrorResponse(403, "Forbidden",
                                     "Read-only access: Viewers cannot edit this note");
        }
        return makeErrorResponse(404, "Not Found", "Note not found");
    }

    HttpResponse res;
    res.status_code = 200;
    res.status_text = "OK";
    res.headers["Content-Type"] = "application/json";
    res.body = json(*note_opt).dump();
    return res;
}

HttpResponse Router::handleDeleteNote(const std::string& id_str,
                                      const std::string& owner_id) const {
    auto id_opt = Utils::parseUUID(id_str);
    if (!id_opt) {
        return makeErrorResponse(400, "Bad Request", "Invalid note ID format");
    }

    bool deleted = store_.deleteNote(*id_opt, owner_id);
    if (!deleted) {
        return makeErrorResponse(404, "Not Found", "Note not found");
    }

    HttpResponse res;
    res.status_code = 204;
    res.status_text = "No Content";
    return res;
}

HttpResponse Router::handlePostAttachment(const std::string& note_id, const std::string& body,
                                          const std::string& owner_id) const {
    auto id_opt = Utils::parseUUID(note_id);
    if (!id_opt) {
        return makeErrorResponse(400, "Bad Request", "Invalid note ID format");
    }

    auto note_opt = store_.getNoteById(*id_opt, owner_id);
    if (!note_opt) {
        return makeErrorResponse(404, "Not Found", "Note not found");
    }

    constexpr long long MAX_USER_ATTACHMENT_QUOTA = 100LL * 1024LL * 1024LL;
    long long current_total_size = store_.getTotalAttachmentSizeForUser(owner_id);
    if (current_total_size >= MAX_USER_ATTACHMENT_QUOTA) {
        return makeErrorResponse(400, "Bad Request",
                                 "User attachment storage quota exceeded (maximum 100MB per user)");
    }

    std::string filename = "attachment";
    auto j = json::parse(body, nullptr, false);
    if (!j.is_discarded() && j.is_object() && j.contains("filename") && j["filename"].is_string()) {
        filename = j["filename"].get<std::string>();
    }

    // Reject unsafe filenames: no path separators, traversal or control chars.
    auto safe_name = Utils::sanitizeFilename(filename);
    if (!safe_name) {
        return makeErrorResponse(400, "Bad Request",
                                 "Invalid filename: path separators, traversal sequences and "
                                 "control characters are not allowed");
    }
    filename = *safe_name;

    std::string attachment_id = Utils::generateUUID();
    std::string key = note_id + "/" + attachment_id + "_" + filename;

    std::string bucket = store_.getBucketName();
    ObjectStore* os = store_.getObjectStore();
    if (!os) {
        return makeErrorResponse(500, "Internal Server Error", "Object storage not configured");
    }

    std::string put_url = os->generatePresignedPutUrl(bucket, key, 300);

    HttpResponse res;
    res.status_code = 200;
    res.status_text = "OK";
    res.headers["Content-Type"] = "application/json";
    res.body = json{{"attachment_id", attachment_id}, {"key", key}, {"url", put_url}}.dump();
    return res;
}

HttpResponse Router::handleCompleteAttachment(const std::string& note_id, const std::string& body,
                                              const std::string& owner_id) const {
    auto id_opt = Utils::parseUUID(note_id);
    if (!id_opt) {
        return makeErrorResponse(400, "Bad Request", "Invalid note ID format");
    }

    auto j = json::parse(body, nullptr, false);
    if (j.is_discarded()) {
        return makeErrorResponse(400, "Bad Request", "Invalid JSON format");
    }

    if (!j.is_object() || !j.contains("attachment_id") || !j.contains("key") ||
        !j.contains("filename") || !j.contains("size") || !j["attachment_id"].is_string() ||
        !j["key"].is_string() || !j["filename"].is_string() || !j["size"].is_number_integer()) {
        return makeErrorResponse(400, "Bad Request", "Missing or invalid fields in metadata");
    }

    std::string attachment_id = j["attachment_id"].get<std::string>();
    std::string key = j["key"].get<std::string>();
    std::string filename = j["filename"].get<std::string>();
    long long size = j["size"].get<long long>();

    auto safe_name = Utils::sanitizeFilename(filename);
    if (!safe_name) {
        return makeErrorResponse(400, "Bad Request",
                                 "Invalid filename: path separators, traversal sequences and "
                                 "control characters are not allowed");
    }
    filename = *safe_name;

    // Reject completions whose bytes live at an attacker-chosen key.
    std::string expected_key = note_id + "/" + attachment_id + "_" + filename;
    if (key != expected_key) {
        return makeErrorResponse(400, "Bad Request", "Attachment key mismatch");
    }

    constexpr long long MAX_USER_ATTACHMENT_QUOTA = 100LL * 1024LL * 1024LL;
    long long current_total_size = store_.getTotalAttachmentSizeForUser(owner_id);
    if (current_total_size + size > MAX_USER_ATTACHMENT_QUOTA) {
        return makeErrorResponse(400, "Bad Request",
                                 "User attachment storage quota exceeded (maximum 100MB per user)");
    }

    std::string bucket = store_.getBucketName();
    bool success =
        store_.addAttachment(attachment_id, *id_opt, bucket, key, size, filename, owner_id);
    if (!success) {
        return makeErrorResponse(404, "Not Found",
                                 "Failed to add attachment (note not found or database error)");
    }

    HttpResponse res;
    res.status_code = 201;
    res.status_text = "Created";
    res.headers["Content-Type"] = "application/json";
    res.body = json{{"status", "success"}}.dump();
    return res;
}

HttpResponse Router::handleDeleteAttachment(const std::string& note_id,
                                            const std::string& attachment_id,
                                            const std::string& owner_id) const {
    auto note_id_opt = Utils::parseUUID(note_id);
    auto att_id_opt = Utils::parseUUID(attachment_id);
    if (!note_id_opt || !att_id_opt) {
        return makeErrorResponse(400, "Bad Request", "Invalid UUID format");
    }

    bool deleted = store_.deleteAttachment(*att_id_opt, *note_id_opt, owner_id);
    if (!deleted) {
        return makeErrorResponse(404, "Not Found", "Attachment or note not found");
    }

    HttpResponse res;
    res.status_code = 204;
    res.status_text = "No Content";
    return res;
}

HttpResponse Router::handleLogout(const HttpRequest& req) const {
    std::string token;
    auto auth_it = req.headers.find("authorization");
    if (auth_it != req.headers.end() && auth_it->second.rfind("Bearer ", 0) == 0) {
        token = auth_it->second.substr(7);
    }

    auto claims = Crypto::verifyTokenClaims(token, auth_service_.getSecret());
    if (!claims || claims->jti.empty()) {
        HttpResponse res;
        res.status_code = 200;
        res.status_text = "OK";
        res.headers["Content-Type"] = "application/json";
        res.body = json{{"status", "logged_out"}}.dump();
        return res;
    }

    if (cache_) {
        long long ttl = claims->exp - std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count();
        if (ttl > 0) {
            cache_->set(std::string("jwt:denylist:") + claims->jti, "1",
                        static_cast<int>(ttl) + 60);  // clock-skew margin
        }
    }

    HttpResponse res;
    res.status_code = 200;
    res.status_text = "OK";
    res.headers["Content-Type"] = "application/json";
    res.body = json{{"status", "logged_out"}}.dump();
    return res;
}

HttpResponse Router::handleRealtimeTicket(const std::string& user_id) const {
    if (!cache_) {
        return makeErrorResponse(503, "Service Unavailable", "Ticket store unavailable");
    }

    std::string ticket = Crypto::randomHex(32);
    constexpr int TICKET_TTL_SECONDS = 30;
    cache_->set("rt:ticket:" + ticket, user_id, TICKET_TTL_SECONDS);

    HttpResponse res;
    res.status_code = 200;
    res.status_text = "OK";
    res.headers["Content-Type"] = "application/json";
    res.body =
        json{{"ticket", ticket}, {"expires_in", TICKET_TTL_SECONDS}, {"token_type", "ticket"}}
            .dump();
    return res;
}

HttpResponse Router::handleEvents([[maybe_unused]] HttpRequest& req) const {
    HttpResponse res;
    res.status_code = 200;
    res.status_text = "OK";
    res.headers["Content-Type"] = "text/event-stream";
    res.headers["Cache-Control"] = "no-cache";
    res.headers["Connection"] = "keep-alive";
    res.headers["Access-Control-Allow-Origin"] = "*";
    res.is_sse = true;
    return res;
}

HttpResponse Router::handleShareNote(const std::string& note_id, const std::string& body,
                                     const std::string& owner_id) const {
    auto id_opt = Utils::parseUUID(note_id);
    if (!id_opt) {
        return makeErrorResponse(400, "Bad Request", "Invalid note ID format");
    }

    auto note_opt = store_.getNoteById(*id_opt, owner_id);
    if (!note_opt) {
        return makeErrorResponse(404, "Not Found", "Note not found");
    }

    if (note_opt->permission != "owner") {
        return makeErrorResponse(403, "Forbidden", "Only the note owner can manage note shares");
    }

    auto j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return makeErrorResponse(400, "Bad Request", "Invalid JSON format");
    }

    std::string target_user_id;
    if (j.contains("target_email") && j["target_email"].is_string()) {
        std::string target_email = j["target_email"].get<std::string>();
        auto target_user = auth_service_.getUserByEmail(target_email);
        if (!target_user) {
            return makeErrorResponse(404, "Not Found", "Target user not found");
        }
        target_user_id = target_user->id;
    } else if (j.contains("email") && j["email"].is_string()) {
        std::string target_email = j["email"].get<std::string>();
        auto target_user = auth_service_.getUserByEmail(target_email);
        if (!target_user) {
            return makeErrorResponse(404, "Not Found", "Target user not found");
        }
        target_user_id = target_user->id;
    } else if (j.contains("target_user_id") && j["target_user_id"].is_string()) {
        target_user_id = j["target_user_id"].get<std::string>();
    } else {
        return makeErrorResponse(400, "Bad Request", "Missing recipient email or target_user_id");
    }

    std::string permission = "editor";
    if (j.contains("permission") && j["permission"].is_string()) {
        std::string req_perm = j["permission"].get<std::string>();
        if (req_perm == "viewer" || req_perm == "editor") {
            permission = req_perm;
        }
    }

    bool shared = store_.shareNote(*id_opt, target_user_id, owner_id, permission);
    if (!shared) {
        return makeErrorResponse(500, "Internal Server Error", "Failed to share note");
    }

    if (event_bus_) {
        std::string sender_email = "User";
        auto sender_user = auth_service_.getUserById(owner_id);
        if (sender_user) {
            sender_email = sender_user->email;
        }

        json event_json = {{"type", "note_shared"},
                           {"note_id", *id_opt},
                           {"title", note_opt->title},
                           {"sender_id", owner_id},
                           {"sender_email", sender_email},
                           {"permission", permission},
                           {"message", "A note was shared with you (" + permission + " access)"}};
        event_bus_->publish(target_user_id, event_json.dump());
    }

    HttpResponse res;
    res.status_code = 200;
    res.status_text = "OK";
    res.headers["Content-Type"] = "application/json";
    res.body =
        json{{"status", "success"}, {"target_user_id", target_user_id}, {"permission", permission}}
            .dump();
    return res;
}

HttpResponse Router::handleGetNoteShares(const std::string& note_id,
                                         const std::string& owner_id) const {
    auto id_opt = Utils::parseUUID(note_id);
    if (!id_opt) {
        return makeErrorResponse(400, "Bad Request", "Invalid note ID format");
    }

    auto shares = store_.getNoteShares(*id_opt, owner_id);
    HttpResponse res;
    res.status_code = 200;
    res.status_text = "OK";
    res.headers["Content-Type"] = "application/json";
    res.body = json(shares).dump();
    return res;
}

HttpResponse Router::handleDeleteNoteShare(const std::string& note_id,
                                           const std::string& target_user_id,
                                           const std::string& owner_id) const {
    auto note_id_opt = Utils::parseUUID(note_id);
    auto target_user_id_opt = Utils::parseUUID(target_user_id);
    if (!note_id_opt || !target_user_id_opt) {
        return makeErrorResponse(400, "Bad Request", "Invalid UUID format");
    }

    bool removed = store_.removeNoteShare(*note_id_opt, *target_user_id_opt, owner_id);
    if (!removed) {
        return makeErrorResponse(404, "Not Found", "Share record or note not found");
    }

    HttpResponse res;
    res.status_code = 204;
    res.status_text = "No Content";
    return res;
}

HttpResponse Router::handleNoteWebSocket(const std::string& note_id, HttpRequest& req) const {
    auto id_opt = Utils::parseUUID(note_id);
    if (!id_opt) {
        return makeErrorResponse(400, "Bad Request", "Invalid note ID format");
    }

    auto note_opt = store_.getNoteById(*id_opt, req.user_id);
    if (!note_opt) {
        return makeErrorResponse(404, "Not Found", "Note not found");
    }

    std::string sec_key;
    for (const auto& [k, v] : req.headers) {
        std::string lk = k;
        std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);
        if (lk == "sec-websocket-key") {
            sec_key = v;
            break;
        }
    }

    if (sec_key.empty()) {
        return makeErrorResponse(400, "Bad Request", "Missing Sec-WebSocket-Key header");
    }

    std::string accept_input = sec_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string sha1_hash = Crypto::sha1(accept_input);
    std::string accept_key = Crypto::base64_encode(sha1_hash);

    std::string user_email = "User";
    auto user_info = auth_service_.getUserById(req.user_id);
    if (user_info) {
        user_email = user_info->email;
    }

    HttpResponse res;
    res.status_code = 101;
    res.status_text = "Switching Protocols";
    res.headers["Upgrade"] = "websocket";
    res.headers["Connection"] = "Upgrade";
    res.headers["Sec-WebSocket-Accept"] = accept_key;
    res.headers["X-Note-Id"] = note_id;
    res.headers["X-User-Email"] = user_email;
    // Snapshotted at join; registry denies relays from viewers.
    bool is_editor = note_opt->permission == "owner" || note_opt->permission == "editor";
    res.headers["X-Note-Editor"] = is_editor ? "1" : "0";
    res.is_websocket = true;
    return res;
}

HttpResponse Router::handleExportPdf(const std::string& note_id, const std::string& user_id) const {
    auto note_opt = store_.getNoteById(note_id, user_id);
    if (!note_opt) {
        return makeErrorResponse(404, "Not Found", "Note not found or unauthorized");
    }

    const char* amqp_env = std::getenv("RABBITMQ_URL");
    std::string amqp_url = amqp_env ? amqp_env : "amqp://guest:guest@rabbitmq:5672/";

    notenest::RabbitMQClient rabbit_client(amqp_url);
    std::string result_json =
        rabbit_client.requestPdfExport(note_opt->id, note_opt->title, note_opt->content, user_id);

    HttpResponse res;
    res.status_code = 200;
    res.status_text = "OK";
    res.headers["Content-Type"] = "application/json";
    res.body = result_json;
    return res;
}
