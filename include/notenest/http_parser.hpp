#ifndef HTTP_PARSER_HPP
#define HTTP_PARSER_HPP

#include <cstddef>
#include <notenest/http_types.hpp>
#include <string>

// Hand-rolled hardened HTTP/1.1 request parser. Rejects smuggling shapes:
// duplicate Content-Length, any Transfer-Encoding, oversized headers/bodies.
class HttpParser {
public:
    enum class Result { Ok, Incomplete, Error };

    static constexpr size_t kMaxHeaderBytes = 16u * 1024u;
    static constexpr size_t kMaxBodyBytes = 8u * 1024u * 1024u;

    // Parses raw data into an HttpRequest. Error means the stream is malformed
    // or hostile and the connection must be closed after a 400 response.
    static Result parse(const std::string& raw_data, HttpRequest& req, size_t& bytes_consumed);
};

#endif
