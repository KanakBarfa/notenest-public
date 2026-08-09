#ifndef HTTP_PARSER_HPP
#define HTTP_PARSER_HPP

#include <notenest/http_types.hpp>
#include <string>

// Hand-rolled HTTP/1.1 request parser.
class HttpParser {
public:
    // Parses raw data into an HttpRequest. Returns true if request is complete.
    static bool parse(const std::string& raw_data, HttpRequest& req, size_t& bytes_consumed);
};

#endif
