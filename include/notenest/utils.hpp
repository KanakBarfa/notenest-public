#ifndef UTILS_HPP
#define UTILS_HPP

#include <optional>
#include <string>

namespace Utils {

// Computes SHA-256 hash of string input returning lower hex string.
std::string sha256_hex(const std::string& input);

// Converts binary string to hex representation.
std::string to_hex(const std::string& input);

// Generates a random version-4 UUID string.
std::string generateUUID();

// Validates and returns UUID string if valid.
std::optional<std::string> parseUUID(const std::string& id_str);

// Rejects unsafe attachment filenames (separators, traversal, control chars).
std::optional<std::string> sanitizeFilename(const std::string& name);

// Standard URL encoding helper.
std::string urlEncode(const std::string& str);

// S3 URI path encoding helper.
std::string s3UriEncode(const std::string& str, bool encodeSlash);

// Counts whitespace-separated words in a text string.
size_t countWords(const std::string& text);

}  // namespace Utils

#endif
