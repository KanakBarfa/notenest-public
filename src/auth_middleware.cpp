#include <notenest/auth_middleware.hpp>
#include <notenest/crypto.hpp>
#include <notenest/observability.hpp>

namespace {
constexpr const char* kDenylistPrefix = "jwt:denylist:";
}

AuthMiddleware::AuthMiddleware(AuthService& auth_service, Cache* denylist_cache)
    : auth_service_(auth_service), denylist_cache_(denylist_cache) {}

std::optional<std::string> AuthMiddleware::authenticate(const HttpRequest& req) const {
    // Authorization header only; no query-string tokens.
    std::string token;
    auto auth_it = req.headers.find("authorization");
    if (auth_it != req.headers.end() && auth_it->second.rfind("Bearer ", 0) == 0) {
        token = auth_it->second.substr(7);
    }

    if (token.empty()) {
        Observability::logJson("DEBUG", "Auth rejected: missing bearer token");
        return std::nullopt;
    }

    auto claims = Crypto::verifyTokenClaims(token, auth_service_.getSecret());
    if (!claims) {
        Observability::logJson("DEBUG",
                               "Auth rejected: invalid or expired token (signature/exp/iss/alg)");
        return std::nullopt;
    }

    if (denylist_cache_ && !claims->jti.empty()) {
        if (denylist_cache_->get(std::string(kDenylistPrefix) + claims->jti)) {
            Observability::logJson("DEBUG", "Auth rejected: token revoked (jti in denylist)");
            return std::nullopt;
        }
    }

    return claims->user_id;
}
