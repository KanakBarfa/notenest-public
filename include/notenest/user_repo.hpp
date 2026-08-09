#ifndef USER_REPO_HPP
#define USER_REPO_HPP

#include <notenest/user.hpp>
#include <optional>
#include <string>

// Abstract repository interface for user persistence.
class UserRepository {
public:
    virtual ~UserRepository() = default;

    virtual std::optional<User> createUser(const std::string& email,
                                           const std::string& password_hash) = 0;
    virtual std::optional<User> getUserByEmail(const std::string& email) = 0;
    virtual std::optional<User> getUserById(const std::string& id) = 0;
};

// PostgreSQL-backed user repository implementation.
class PgUserRepository : public UserRepository {
public:
    PgUserRepository() = default;

    std::optional<User> createUser(const std::string& email,
                                   const std::string& password_hash) override;
    std::optional<User> getUserByEmail(const std::string& email) override;
    std::optional<User> getUserById(const std::string& id) override;
};

#endif
