#include <algorithm>
#include <cctype>
#include <notenest/http_parser.hpp>
#include <sstream>

// Helper to trim whitespace from a string.
static std::string trim(const std::string& str) {
    auto start =
        std::find_if_not(str.begin(), str.end(), [](unsigned char ch) { return std::isspace(ch); });
    auto end = std::find_if_not(str.rbegin(), str.rend(), [](unsigned char ch) {
                   return std::isspace(ch);
               }).base();
    return (start < end) ? std::string(start, end) : "";
}

// Helper to convert string to lowercase.
static std::string toLower(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return str;
}

// Strict digits-only parse; rejects signs, spaces, overflow, values above cap.
static bool parseContentLength(const std::string& val, size_t cap, size_t& out) {
    if (val.empty() || val.size() > 10) {
        return false;
    }
    unsigned long long n = 0;
    for (char ch : val) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            return false;
        }
        n = n * 10 + static_cast<unsigned long long>(ch - '0');
    }
    if (n > cap) {
        return false;
    }
    out = static_cast<size_t>(n);
    return true;
}

// Connection header token check: comma-separated list, case-insensitive.
static bool connectionToken(const HttpRequest& req, const std::string& token) {
    auto it = req.headers.find("connection");
    if (it == req.headers.end()) {
        return false;
    }
    std::istringstream stream(it->second);
    std::string tok;
    while (std::getline(stream, tok, ',')) {
        if (toLower(trim(tok)) == token) {
            return true;
        }
    }
    return false;
}

HttpParser::Result HttpParser::parse(const std::string& raw_data, HttpRequest& req,
                                     size_t& bytes_consumed) {
    size_t header_end = raw_data.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        // Unbounded header growth: kill the connection instead of buffering on.
        return raw_data.size() > kMaxHeaderBytes ? Result::Error : Result::Incomplete;
    }
    if (header_end > kMaxHeaderBytes) {
        return Result::Error;
    }

    std::string header_part = raw_data.substr(0, header_end);
    std::string line;
    std::istringstream stream(header_part);

    // Request line: exactly METHOD SP TARGET SP VERSION.
    if (!std::getline(stream, line)) {
        return Result::Error;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    std::istringstream req_line_stream(line);
    std::string method_str, path_str, version_str;
    if (!(req_line_stream >> method_str >> path_str >> version_str)) {
        return Result::Error;
    }
    std::string extra;
    if (req_line_stream >> extra) {
        return Result::Error;
    }
    req.method = stringToMethod(method_str);
    if (req.method == HttpMethod::UNKNOWN || path_str.empty() || path_str[0] != '/' ||
        (version_str != "HTTP/1.1" && version_str != "HTTP/1.0")) {
        return Result::Error;
    }
    req.path = path_str;

    // Headers: strict field-name/value grammar, no obs-fold, no duplicates of
    // framing-relevant fields.
    size_t content_length = 0;
    int header_count = 0;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        if (++header_count > 100) {
            return Result::Error;
        }
        if (line[0] == ' ' || line[0] == '\t') {
            return Result::Error;  // obs-fold continuation
        }
        size_t colon = line.find(':');
        if (colon == std::string::npos) {
            return Result::Error;
        }
        std::string key = toLower(trim(line.substr(0, colon)));
        std::string val = trim(line.substr(colon + 1));
        if (key.empty() || key.find(' ') != std::string::npos) {
            return Result::Error;
        }
        for (unsigned char ch : val) {
            if ((ch < 0x20 && ch != '\t') || ch == 0x7f) {
                return Result::Error;
            }
        }

        if (key == "transfer-encoding") {
            // Only Content-Length framing is supported; TE+CL and TE.TE are
            // classic request-smuggling shapes.
            return Result::Error;
        }

        auto [it, inserted] = req.headers.emplace(key, val);
        if (!inserted) {
            if (key == "content-length" || key == "host") {
                return Result::Error;  // conflicting duplicate
            }
            it->second = val;
            continue;
        }
        if (key == "content-length") {
            if (!parseContentLength(val, kMaxBodyBytes, content_length)) {
                return Result::Error;
            }
        }
    }

    size_t total_required = header_end + 4 + content_length;
    if (raw_data.size() < total_required) {
        return Result::Incomplete;
    }

    req.body = raw_data.substr(header_end + 4, content_length);
    bytes_consumed = total_required;

    // Explicit keep-alive semantics per RFC 7230 section 6.3.
    req.keep_alive = version_str == "HTTP/1.1" ? !connectionToken(req, "close")
                                               : connectionToken(req, "keep-alive");
    return Result::Ok;
}
