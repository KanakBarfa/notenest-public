#include <notenest/auth_middleware.hpp>
#include <notenest/crypto.hpp>

AuthMiddleware::AuthMiddleware(AuthService& auth_service) : auth_service_(auth_service) {}

std::optional<std::string> AuthMiddleware::authenticate(const HttpRequest& req) const {
    // Extract token from Authorization header or ?token= query param (SSE/WS).
    std::string token;
    auto auth_it = req.headers.find("authorization");
    if (auth_it != req.headers.end() && auth_it->second.rfind("Bearer ", 0) == 0) {
        token = auth_it->second.substr(7);
    }

    if (token.empty()) {
        size_t qpos = req.path.find('?');
        if (qpos != std::string::npos) {
            std::string query = req.path.substr(qpos + 1);
            for (const auto& prefix : {"token=", "access_token="}) {
                size_t tpos = query.find(prefix);
                if (tpos != std::string::npos) {
                    token = query.substr(tpos + std::string(prefix).length());
                    size_t amp = token.find('&');
                    if (amp != std::string::npos)
                        token = token.substr(0, amp);
                    break;
                }
            }
        }
    }

    if (token.empty())
        return std::nullopt;
    return Crypto::verifyToken(token, auth_service_.getSecret());
}
