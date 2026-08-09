#include <chrono>
#include <notenest/consul_client.hpp>
#include <notenest/grpc_clients.hpp>

AuthGrpcClient::AuthGrpcClient(std::string target_address) {
    ConsulClient consul;
    std::string resolved_target = consul.resolveGrpcTarget(target_address, "auth-service");
    grpc::ChannelArguments args;
    args.SetLoadBalancingPolicyName("round_robin");
    auto channel =
        grpc::CreateCustomChannel(resolved_target, grpc::InsecureChannelCredentials(), args);
    stub_ = notenest::auth::AuthService::NewStub(channel);
}

AuthResult AuthGrpcClient::signup(const std::string& email, const std::string& password,
                                  const std::string& trace_id) {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    if (!trace_id.empty()) {
        context.AddMetadata("trace-id", trace_id);
    }

    notenest::auth::SignupRequest request;
    request.set_email(email);
    request.set_password(password);

    notenest::auth::AuthResponse response;
    grpc::Status status = stub_->Signup(&context, request, &response);

    AuthResult res;
    if (status.ok()) {
        res.success = true;
        res.user_id = response.user_id();
        res.email = response.email();
        res.token = response.token();
    } else {
        res.success = false;
        res.error = status.error_message();
    }
    return res;
}

AuthResult AuthGrpcClient::login(const std::string& email, const std::string& password,
                                 int expiry_seconds, const std::string& trace_id) {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    if (!trace_id.empty()) {
        context.AddMetadata("trace-id", trace_id);
    }

    notenest::auth::LoginRequest request;
    request.set_email(email);
    request.set_password(password);
    request.set_expiry_seconds(expiry_seconds);

    notenest::auth::AuthResponse response;
    grpc::Status status = stub_->Login(&context, request, &response);

    AuthResult res;
    if (status.ok()) {
        res.success = true;
        res.user_id = response.user_id();
        res.email = response.email();
        res.token = response.token();
    } else {
        res.success = false;
        res.error = status.error_message();
    }
    return res;
}

UserGrpcClient::UserGrpcClient(std::string target_address) {
    ConsulClient consul;
    std::string resolved_target = consul.resolveGrpcTarget(target_address, "user-service");
    grpc::ChannelArguments args;
    args.SetLoadBalancingPolicyName("round_robin");
    auto channel =
        grpc::CreateCustomChannel(resolved_target, grpc::InsecureChannelCredentials(), args);
    stub_ = notenest::user::UserService::NewStub(channel);
}

std::optional<UserProfile> UserGrpcClient::getUserProfile(const std::string& user_id,
                                                          const std::string& trace_id) {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
    if (!trace_id.empty()) {
        context.AddMetadata("trace-id", trace_id);
    }

    notenest::user::GetUserProfileRequest request;
    request.set_user_id(user_id);

    notenest::user::UserProfileResponse response;
    grpc::Status status = stub_->GetUserProfile(&context, request, &response);

    if (status.ok()) {
        UserProfile profile;
        profile.user_id = response.user_id();
        profile.email = response.email();
        profile.created_at = response.created_at();
        return profile;
    }
    return std::nullopt;
}

std::vector<UserProfile> UserGrpcClient::getUsersByIDs(const std::vector<std::string>& user_ids,
                                                       const std::string& trace_id) {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
    if (!trace_id.empty()) {
        context.AddMetadata("trace-id", trace_id);
    }

    notenest::user::GetUsersByIDsRequest request;
    for (const auto& id : user_ids) {
        request.add_user_ids(id);
    }

    notenest::user::UsersBatchResponse response;
    grpc::Status status = stub_->GetUsersByIDs(&context, request, &response);

    std::vector<UserProfile> results;
    if (status.ok()) {
        for (const auto& u : response.users()) {
            UserProfile profile;
            profile.user_id = u.user_id();
            profile.email = u.email();
            profile.created_at = u.created_at();
            results.push_back(profile);
        }
    }
    return results;
}

NoteGrpcClient::NoteGrpcClient(std::string target_address) {
    ConsulClient consul;
    std::string resolved_target = consul.resolveGrpcTarget(target_address, "note-service");
    grpc::ChannelArguments args;
    args.SetLoadBalancingPolicyName("ring_hash");
    auto channel =
        grpc::CreateCustomChannel(resolved_target, grpc::InsecureChannelCredentials(), args);
    stub_ = notenest::note::NoteService::NewStub(channel);
}

std::optional<notenest::note::NoteResponse> NoteGrpcClient::createNote(
    const std::string& owner_id, const std::string& title, const std::string& content,
    const std::string& trace_id) {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    if (!trace_id.empty()) {
        context.AddMetadata("trace-id", trace_id);
    }
    context.AddMetadata("user-id", owner_id);

    notenest::note::CreateNoteRequest request;
    request.set_owner_id(owner_id);
    request.set_title(title);
    request.set_content(content);

    notenest::note::NoteResponse response;
    grpc::Status status = stub_->CreateNote(&context, request, &response);

    if (status.ok()) {
        return response;
    }
    return std::nullopt;
}

std::optional<notenest::note::NoteResponse> NoteGrpcClient::getNote(const std::string& note_id,
                                                                    const std::string& user_id,
                                                                    const std::string& trace_id) {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    if (!trace_id.empty()) {
        context.AddMetadata("trace-id", trace_id);
    }
    context.AddMetadata("user-id", user_id);

    notenest::note::GetNoteRequest request;
    request.set_note_id(note_id);
    request.set_requesting_user_id(user_id);

    notenest::note::NoteResponse response;
    grpc::Status status = stub_->GetNote(&context, request, &response);

    if (status.ok()) {
        return response;
    }
    return std::nullopt;
}

std::vector<notenest::note::NoteResponse> NoteGrpcClient::listNotes(const std::string& owner_id,
                                                                    const std::string& trace_id) {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    if (!trace_id.empty()) {
        context.AddMetadata("trace-id", trace_id);
    }
    context.AddMetadata("user-id", owner_id);

    notenest::note::ListNotesRequest request;
    request.set_owner_id(owner_id);

    notenest::note::ListNotesResponse response;
    grpc::Status status = stub_->ListNotes(&context, request, &response);

    std::vector<notenest::note::NoteResponse> notes;
    if (status.ok()) {
        for (const auto& n : response.notes()) {
            notes.push_back(n);
        }
    }
    return notes;
}
