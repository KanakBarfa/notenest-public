#include <notenest/auth_service.hpp>
#include <notenest/crypto.hpp>

AuthService::AuthService(UserRepository& user_repo, std::string jwt_secret)
    : user_repo_(user_repo), jwt_secret_(std::move(jwt_secret)) {}

std::optional<std::string> AuthService::signup(const std::string& email,
                                               const std::string& password) {
    if (email.empty() || password.empty()) {
        return std::nullopt;
    }

    try {
        std::string password_hash = Crypto::hashPassword(password);
        auto user_opt = user_repo_.createUser(email, password_hash);
        if (user_opt) {
            return user_opt->id;
        }
    } catch (...) {
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::string> AuthService::login(const std::string& email, const std::string& password,
                                              int expiry_seconds) {
    if (email.empty() || password.empty()) {
        return std::nullopt;
    }

    auto user_opt = user_repo_.getUserByEmail(email);
    if (!user_opt) {
        return std::nullopt;
    }

    try {
        if (!Crypto::verifyPassword(password, user_opt->password_hash)) {
            return std::nullopt;
        }
        return Crypto::generateToken(user_opt->id, jwt_secret_, expiry_seconds);
    } catch (...) {
        return std::nullopt;
    }
}
