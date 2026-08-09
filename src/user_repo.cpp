#include <notenest/db_pool.hpp>
#include <notenest/user_repo.hpp>
#include <print>

std::optional<User> PgUserRepository::createUser(const std::string& email,
                                                 const std::string& password_hash) {
    DBConnectionGuard guard(DBPoolMode::WRITE);
    PGconn* conn = guard.get();
    if (!conn) {
        return std::nullopt;
    }

    const char* query =
        "INSERT INTO users (email, password_hash) VALUES ($1, $2) RETURNING id, email, "
        "password_hash";
    const char* paramValues[2] = {email.c_str(), password_hash.c_str()};
    PGresult* res = PQexecParams(conn, query, 2, nullptr, paramValues, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::println(stderr, "DB Error creating user: {}", PQerrorMessage(conn));
        PQclear(res);
        return std::nullopt;
    }

    if (PQntuples(res) == 0) {
        PQclear(res);
        return std::nullopt;
    }

    User user;
    user.id = PQgetvalue(res, 0, 0);
    user.email = PQgetvalue(res, 0, 1);
    user.password_hash = PQgetvalue(res, 0, 2);

    PQclear(res);
    return user;
}

std::optional<User> PgUserRepository::getUserByEmail(const std::string& email) {
    DBConnectionGuard guard(DBPoolMode::READ);
    PGconn* conn = guard.get();
    if (!conn) {
        return std::nullopt;
    }

    const char* query = "SELECT id, email, password_hash FROM users WHERE email = $1";
    const char* paramValues[1] = {email.c_str()};
    PGresult* res = PQexecParams(conn, query, 1, nullptr, paramValues, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::println(stderr, "DB Error fetching user: {}", PQerrorMessage(conn));
        PQclear(res);
        return std::nullopt;
    }

    if (PQntuples(res) == 0) {
        PQclear(res);
        return std::nullopt;
    }

    User user;
    user.id = PQgetvalue(res, 0, 0);
    user.email = PQgetvalue(res, 0, 1);
    user.password_hash = PQgetvalue(res, 0, 2);

    PQclear(res);
    return user;
}

std::optional<User> PgUserRepository::getUserById(const std::string& id) {
    DBConnectionGuard guard(DBPoolMode::READ);
    PGconn* conn = guard.get();
    if (!conn) {
        return std::nullopt;
    }

    const char* query = "SELECT id, email, password_hash FROM users WHERE id = $1::uuid";
    const char* paramValues[1] = {id.c_str()};
    PGresult* res = PQexecParams(conn, query, 1, nullptr, paramValues, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::println(stderr, "DB Error fetching user by ID: {}", PQerrorMessage(conn));
        PQclear(res);
        return std::nullopt;
    }

    if (PQntuples(res) == 0) {
        PQclear(res);
        return std::nullopt;
    }

    User user;
    user.id = PQgetvalue(res, 0, 0);
    user.email = PQgetvalue(res, 0, 1);
    user.password_hash = PQgetvalue(res, 0, 2);

    PQclear(res);
    return user;
}
