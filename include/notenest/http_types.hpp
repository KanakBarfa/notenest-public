#ifndef HTTP_TYPES_HPP
#define HTTP_TYPES_HPP

#include <string>
#include <unordered_map>

// HTTP methods supported by the application.
enum class HttpMethod { GET, POST, PUT, DELETE, OPTIONS, UNKNOWN };

// Converts HTTP method enum to string representation.
inline std::string methodToString(HttpMethod method) {
    switch (method) {
        case HttpMethod::GET:
            return "GET";
        case HttpMethod::POST:
            return "POST";
        case HttpMethod::PUT:
            return "PUT";
        case HttpMethod::DELETE:
            return "DELETE";
        case HttpMethod::OPTIONS:
            return "OPTIONS";
        default:
            return "UNKNOWN";
    }
}

// Converts string representation to HTTP method enum.
inline HttpMethod stringToMethod(const std::string& methodStr) {
    if (methodStr == "GET")
        return HttpMethod::GET;
    if (methodStr == "POST")
        return HttpMethod::POST;
    if (methodStr == "PUT")
        return HttpMethod::PUT;
    if (methodStr == "DELETE")
        return HttpMethod::DELETE;
    if (methodStr == "OPTIONS")
        return HttpMethod::OPTIONS;
    return HttpMethod::UNKNOWN;
}

// Represents parsed HTTP request details.
struct HttpRequest {
    HttpMethod method = HttpMethod::UNKNOWN;
    std::string path;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    std::string user_id;
    bool keep_alive = true;  // computed by the parser from version + Connection
};

// Represents HTTP response details.
struct HttpResponse {
    int status_code = 200;
    std::string status_text = "OK";
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    bool is_sse = false;
    bool is_websocket = false;
    bool keep_alive = true;

    // Serializes the response to a raw HTTP response string.
    std::string toString() const {
        std::string res = "HTTP/1.1 " + std::to_string(status_code) + " " + status_text + "\r\n";
        for (const auto& [key, value] : headers) {
            res += key + ": " + value + "\r\n";
        }
        if (is_sse) {
            if (headers.find("Transfer-Encoding") == headers.end() &&
                headers.find("transfer-encoding") == headers.end()) {
                res += "Transfer-Encoding: chunked\r\n";
            }
        } else if (is_websocket) {
            // WebSockets upgrade header handles framing without Content-Length
        } else {
            res += "Connection: ";
            res += keep_alive ? "keep-alive" : "close";
            res += "\r\n";
            res += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        }
        res += "\r\n";
        res += body;
        return res;
    }
};

#endif
