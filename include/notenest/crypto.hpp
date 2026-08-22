#ifndef CRYPTO_HPP
#define CRYPTO_HPP

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <sodium.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace Crypto {

static constexpr int MAX_TOKEN_TTL_SECONDS = 86400;  // Server-capped token lifetime (24h).

// Initializes libsodium library.
inline void init() {
    if (sodium_init() < 0) {
        throw std::runtime_error("libsodium initialization failed");
    }
}

// Returns cryptographically secure random bytes as a lowercase hex string.
inline std::string randomHex(size_t num_bytes) {
    static const char* hex = "0123456789abcdef";
    std::vector<unsigned char> buf(num_bytes);
    randombytes_buf(buf.data(), buf.size());
    std::string out;
    out.reserve(num_bytes * 2);
    for (unsigned char b : buf) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0x0F]);
    }
    return out;
}

static const std::string BASE64_CHARS =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

// Encodes string to standard Base64.
inline std::string base64_encode(const std::string& in) {
    std::string out;
    int val = 0, valb = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(BASE64_CHARS[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6)
        out.push_back(BASE64_CHARS[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4)
        out.push_back('=');
    return out;
}

// Decodes standard Base64 string.
inline std::string base64_decode(const std::string& in) {
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++)
        T[BASE64_CHARS[i]] = i;

    std::string out;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (T[c] == -1)
            break;
        val = (val << 6) + T[c];
        valb += 6;
        while (valb >= 0) {
            out.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

// Encodes string to Base64Url format.
inline std::string base64url_encode(const std::string& in) {
    std::string out = base64_encode(in);
    std::replace(out.begin(), out.end(), '+', '-');
    std::replace(out.begin(), out.end(), '/', '_');
    out.erase(std::remove(out.begin(), out.end(), '='), out.end());
    return out;
}

// Decodes Base64Url formatted string.
inline std::string base64url_decode(const std::string& in) {
    std::string out = in;
    std::replace(out.begin(), out.end(), '-', '+');
    std::replace(out.begin(), out.end(), '_', '/');
    while (out.length() % 4) {
        out.push_back('=');
    }
    return base64_decode(out);
}

// Computes HMAC-SHA256 signature of data using secret key.
inline std::string hmac_sha256(const std::string& data, const std::string& key) {
    unsigned char out[32];
    size_t out_len = 0;
    unsigned char* res = EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr, key.data(),
                                   key.size(), reinterpret_cast<const unsigned char*>(data.data()),
                                   data.size(), out, sizeof(out), &out_len);
    if (!res || out_len != 32) {
        throw std::runtime_error("HMAC SHA256 failed");
    }
    return std::string(reinterpret_cast<char*>(out), out_len);
}

// Computes SHA1 hash of input data.
inline std::string sha1(const std::string& data) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        throw std::runtime_error("SHA1 CTX allocation failed");
    }
    if (EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr) <= 0 ||
        EVP_DigestUpdate(ctx, data.data(), data.size()) <= 0 ||
        EVP_DigestFinal_ex(ctx, hash, &hash_len) <= 0) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("SHA1 digest calculation failed");
    }
    EVP_MD_CTX_free(ctx);
    return std::string(reinterpret_cast<char*>(hash), hash_len);
}

// Hashes password using Argon2id.
inline std::string hashPassword(const std::string& password) {
    char hash[crypto_pwhash_STRBYTES];
    if (crypto_pwhash_str(hash, password.c_str(), password.length(),
                          crypto_pwhash_OPSLIMIT_INTERACTIVE,
                          crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
        throw std::runtime_error("Password hashing failed");
    }
    return std::string(hash);
}

// Verifies password against Argon2id hash.
inline bool verifyPassword(const std::string& password, const std::string& hash) {
    if (hash.length() >= crypto_pwhash_STRBYTES) {
        return false;
    }
    return crypto_pwhash_str_verify(hash.c_str(), password.c_str(), password.length()) == 0;
}

// Generates JWT token with user_id, jti and capped expiry.
inline std::string generateToken(const std::string& user_id, const std::string& secret,
                                 int expiry_seconds) {
    if (expiry_seconds <= 0) {
        expiry_seconds = MAX_TOKEN_TTL_SECONDS;
    }
    expiry_seconds = std::min(expiry_seconds, MAX_TOKEN_TTL_SECONDS);

    nlohmann::json header = {{"alg", "HS256"}, {"typ", "JWT"}};
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
    nlohmann::json payload = {{"user_id", user_id},
                              {"iss", "notenest"},
                              {"jti", randomHex(16)},
                              {"exp", now + expiry_seconds}};

    std::string header_enc = base64url_encode(header.dump());
    std::string payload_enc = base64url_encode(payload.dump());

    std::string signing_input = header_enc + "." + payload_enc;
    std::string signature = hmac_sha256(signing_input, secret);
    std::string signature_enc = base64url_encode(signature);

    return signing_input + "." + signature_enc;
}

// Verified token claims.
struct TokenClaims {
    std::string user_id;
    std::string jti;
    long long exp = 0;
};

// Strict JWT verification (HS256 only, constant-time compare, iss/exp checks).
inline std::optional<TokenClaims> verifyTokenClaims(const std::string& token,
                                                    const std::string& secret) {
    size_t dot1 = token.find('.');
    if (dot1 == std::string::npos)
        return std::nullopt;
    size_t dot2 = token.find('.', dot1 + 1);
    if (dot2 == std::string::npos)
        return std::nullopt;
    if (token.find('.', dot2 + 1) != std::string::npos)
        return std::nullopt;

    std::string header_enc = token.substr(0, dot1);
    std::string payload_enc = token.substr(dot1 + 1, dot2 - dot1 - 1);
    std::string signature_enc = token.substr(dot2 + 1);

    try {
        auto header = nlohmann::json::parse(base64url_decode(header_enc));
        if (!header.is_object() || !header.contains("alg") || header["alg"] != "HS256") {
            return std::nullopt;  // HS256 only.
        }

        std::string signing_input = header_enc + "." + payload_enc;
        std::string expected_sig = hmac_sha256(signing_input, secret);
        std::string provided_sig = base64url_decode(signature_enc);

        if (provided_sig.size() != expected_sig.size() ||
            CRYPTO_memcmp(provided_sig.data(), expected_sig.data(), expected_sig.size()) != 0) {
            return std::nullopt;
        }

        auto payload = nlohmann::json::parse(base64url_decode(payload_enc));
        if (!payload.is_object() || !payload.contains("user_id") || !payload.contains("exp") ||
            !payload.contains("iss")) {
            return std::nullopt;
        }
        if (!payload["user_id"].is_string() || !payload["iss"].is_string() ||
            payload["iss"].get<std::string>() != "notenest") {
            return std::nullopt;
        }

        TokenClaims claims;
        claims.user_id = payload["user_id"].get<std::string>();
        if (claims.user_id.empty()) {
            return std::nullopt;
        }
        claims.exp = payload["exp"].get<long long>();

        auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
        if (now > claims.exp) {
            return std::nullopt;
        }

        if (payload.contains("jti") && payload["jti"].is_string()) {
            claims.jti = payload["jti"].get<std::string>();
        }

        return claims;
    } catch (...) {
        return std::nullopt;
    }
}

// Verifies JWT token and returns user_id if valid, otherwise nullopt.
inline std::optional<std::string> verifyToken(const std::string& token, const std::string& secret) {
    auto claims = verifyTokenClaims(token, secret);
    if (!claims) {
        return std::nullopt;
    }
    return claims->user_id;
}

}  // namespace Crypto

#endif
