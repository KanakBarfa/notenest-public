#ifndef GRPC_CLIENTS_HPP
#define GRPC_CLIENTS_HPP

#include <grpcpp/grpcpp.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "attachment.grpc.pb.h"
#include "auth.grpc.pb.h"
#include "note.grpc.pb.h"
#include "user.grpc.pb.h"

struct AuthResult {
    bool success;
    std::string user_id;
    std::string email;
    std::string token;
    std::string error;
};

struct UserProfile {
    std::string user_id;
    std::string email;
    std::string created_at;
};

class AuthGrpcClient {
public:
    explicit AuthGrpcClient(std::string target_address = "auth:50051");

    AuthResult signup(const std::string& email, const std::string& password,
                      const std::string& trace_id = "");
    AuthResult login(const std::string& email, const std::string& password,
                     int expiry_seconds = 3600, const std::string& trace_id = "");

private:
    std::unique_ptr<notenest::auth::AuthService::Stub> stub_;
};

class UserGrpcClient {
public:
    explicit UserGrpcClient(std::string target_address = "user:50052");

    // requester_id scopes returned emails to note relationships.
    std::optional<UserProfile> getUserProfile(const std::string& user_id,
                                              const std::string& trace_id = "",
                                              const std::string& requester_id = "");
    std::vector<UserProfile> getUsersByIDs(const std::vector<std::string>& user_ids,
                                           const std::string& trace_id = "",
                                           const std::string& requester_id = "");

private:
    std::unique_ptr<notenest::user::UserService::Stub> stub_;
};

class NoteGrpcClient {
public:
    explicit NoteGrpcClient(std::string target_address = "app:50053");

    std::optional<notenest::note::NoteResponse> createNote(const std::string& owner_id,
                                                           const std::string& title,
                                                           const std::string& content,
                                                           const std::string& trace_id = "");
    std::optional<notenest::note::NoteResponse> getNote(const std::string& note_id,
                                                        const std::string& user_id,
                                                        const std::string& trace_id = "");
    std::vector<notenest::note::NoteResponse> listNotes(const std::string& owner_id,
                                                        const std::string& trace_id = "");

private:
    std::unique_ptr<notenest::note::NoteService::Stub> stub_;
};

#endif
