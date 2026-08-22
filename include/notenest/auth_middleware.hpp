#ifndef AUTH_MIDDLEWARE_HPP
#define AUTH_MIDDLEWARE_HPP

#include <notenest/auth_service.hpp>
#include <notenest/cache.hpp>
#include <notenest/http_types.hpp>
#include <optional>
#include <string>

// Bearer JWT validation with Redis jti denylist enforcement.
class AuthMiddleware {
public:
    explicit AuthMiddleware(AuthService& auth_service, Cache* denylist_cache = nullptr);

    // Extracts and verifies Bearer token from request. Returns user ID on success.
    std::optional<std::string> authenticate(const HttpRequest& req) const;

private:
    AuthService& auth_service_;
    Cache* denylist_cache_;
};

#endif
