#ifndef AUTH_MIDDLEWARE_HPP
#define AUTH_MIDDLEWARE_HPP

#include <notenest/auth_service.hpp>
#include <notenest/http_types.hpp>
#include <optional>
#include <string>

// Middleware for extracting and validating Bearer JWT tokens.
class AuthMiddleware {
public:
    explicit AuthMiddleware(AuthService& auth_service);

    // Extracts and verifies Bearer token from request. Returns user ID on success.
    std::optional<std::string> authenticate(const HttpRequest& req) const;

private:
    AuthService& auth_service_;
};

#endif
