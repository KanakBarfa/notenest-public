#ifndef AUTH_SERVICE_HPP
#define AUTH_SERVICE_HPP

#include <notenest/user_repo.hpp>
#include <optional>
#include <string>

// Authentication and user credential verification service.
class AuthService {
public:
    AuthService(UserRepository& user_repo, std::string jwt_secret);

    // Creates a new user. Returns user ID on success, nullopt on duplicate/failure.
    std::optional<std::string> signup(const std::string& email, const std::string& password);

    // Logs in a user and returns a JWT token. Returns nullopt on invalid credentials.
    std::optional<std::string> login(const std::string& email, const std::string& password,
                                     int expiry_seconds = 3600);

    // Looks up a user by email.
    std::optional<User> getUserByEmail(const std::string& email) const {
        return user_repo_.getUserByEmail(email);
    }

    // Looks up a user by ID.
    std::optional<User> getUserById(const std::string& id) const {
        return user_repo_.getUserById(id);
    }

    const std::string& getSecret() const {
        return jwt_secret_;
    }

private:
    UserRepository& user_repo_;
    std::string jwt_secret_;
};

#endif
