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

bool HttpParser::parse(const std::string& raw_data, HttpRequest& req, size_t& bytes_consumed) {
    size_t header_end = raw_data.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return false;
    }

    std::string header_part = raw_data.substr(0, header_end);
    std::string line;
    std::istringstream stream(header_part);

    // Parse request line.
    if (!std::getline(stream, line)) {
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    std::istringstream req_line_stream(line);
    std::string method_str, path_str, version_str;
    if (!(req_line_stream >> method_str >> path_str >> version_str)) {
        return false;
    }

    req.method = stringToMethod(method_str);
    req.path = path_str;

    // Parse headers.
    size_t content_length = 0;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

        size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }

        std::string key = toLower(trim(line.substr(0, colon)));
        std::string val = trim(line.substr(colon + 1));
        req.headers[key] = val;

        if (key == "content-length") {
            try {
                content_length = std::stoul(val);
            } catch (...) {
                content_length = 0;
            }
        }
    }

    size_t total_required = header_end + 4 + content_length;
    if (raw_data.size() < total_required) {
        return false;
    }

    req.body = raw_data.substr(header_end + 4, content_length);
    bytes_consumed = total_required;
    return true;
}
