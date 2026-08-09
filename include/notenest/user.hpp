#ifndef USER_HPP
#define USER_HPP

#include <string>

// Represents a user entity.
struct User {
    std::string id;
    std::string email;
    std::string password_hash;
};

#endif
