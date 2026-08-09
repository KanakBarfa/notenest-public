#include <openssl/evp.h>

#include <cctype>
#include <iomanip>
#include <notenest/utils.hpp>
#include <random>
#include <sstream>
#include <stdexcept>

namespace Utils {

std::string sha256_hex(const std::string& input) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;

    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr) {
        throw std::runtime_error("Failed to create EVP MD context");
    }

    if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(context);
        throw std::runtime_error("DigestInit failed");
    }

    if (EVP_DigestUpdate(context, input.c_str(), input.length()) != 1) {
        EVP_MD_CTX_free(context);
        throw std::runtime_error("DigestUpdate failed");
    }

    if (EVP_DigestFinal_ex(context, hash, &hash_len) != 1) {
        EVP_MD_CTX_free(context);
        throw std::runtime_error("DigestFinal failed");
    }

    EVP_MD_CTX_free(context);

    std::stringstream ss;
    for (unsigned int i = 0; i < hash_len; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return ss.str();
}

std::string to_hex(const std::string& input) {
    std::stringstream ss;
    for (unsigned char c : input) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
    }
    return ss.str();
}

std::string generateUUID() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    std::uniform_int_distribution<> dis2(8, 11);

    std::stringstream ss;
    ss << std::hex;
    for (int i = 0; i < 8; i++)
        ss << dis(gen);
    ss << "-";
    for (int i = 0; i < 4; i++)
        ss << dis(gen);
    ss << "-4";
    for (int i = 0; i < 3; i++)
        ss << dis(gen);
    ss << "-";
    ss << dis2(gen);
    for (int i = 0; i < 3; i++)
        ss << dis(gen);
    ss << "-";
    for (int i = 0; i < 12; i++)
        ss << dis(gen);
    return ss.str();
}

std::optional<std::string> parseUUID(const std::string& id_str) {
    if (id_str.length() != 36)
        return std::nullopt;
    for (size_t i = 0; i < 36; ++i) {
        char c = id_str[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-')
                return std::nullopt;
        } else {
            if (!std::isxdigit(static_cast<unsigned char>(c)))
                return std::nullopt;
        }
    }
    return id_str;
}

std::string urlEncode(const std::string& str) {
    std::stringstream ss;
    for (unsigned char c : str) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            ss << c;
        } else {
            ss << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<int>(c);
        }
    }
    return ss.str();
}

std::string s3UriEncode(const std::string& str, bool encodeSlash) {
    std::stringstream ss;
    for (unsigned char c : str) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            ss << c;
        } else if (c == '/') {
            if (encodeSlash) {
                ss << "%2F";
            } else {
                ss << '/';
            }
        } else {
            ss << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<int>(c);
        }
    }
    return ss.str();
}

size_t countWords(const std::string& text) {
    size_t count = 0;
    bool in_word = false;
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            in_word = false;
        } else if (!in_word) {
            in_word = true;
            ++count;
        }
    }
    return count;
}

}  // namespace Utils
