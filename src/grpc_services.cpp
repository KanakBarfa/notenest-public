#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/health_check_service_interface.h>

#include <chrono>
#include <iostream>
#include <notenest/grpc_services.hpp>
#include <notenest/utils.hpp>

NoteServiceImpl::NoteServiceImpl(NoteStore& note_store, UserRepository& user_repo)
    : note_store_(note_store), user_repo_(user_repo) {}

grpc::Status NoteServiceImpl::CreateNote(grpc::ServerContext*,
                                         const notenest::note::CreateNoteRequest* request,
                                         notenest::note::NoteResponse* response) {
    if (request->owner_id().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Owner ID required");
    }
    Note note = note_store_.createNote(request->title(), request->content(), request->owner_id());
    response->set_id(note.id);
    response->set_title(note.title);
    response->set_content(note.content);
    response->set_owner_id(note.owner_id);
    response->set_created_at(note.created_at);
    response->set_updated_at(note.created_at);
    return grpc::Status::OK;
}

grpc::Status NoteServiceImpl::GetNote(grpc::ServerContext*,
                                      const notenest::note::GetNoteRequest* request,
                                      notenest::note::NoteResponse* response) {
    auto note_opt = note_store_.getNoteById(request->note_id(), request->requesting_user_id());
    if (!note_opt) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "Note not found or unauthorized");
    }
    response->set_id(note_opt->id);
    response->set_title(note_opt->title);
    response->set_content(note_opt->content);
    response->set_owner_id(note_opt->owner_id);
    response->set_created_at(note_opt->created_at);
    response->set_updated_at(note_opt->created_at);
    return grpc::Status::OK;
}

grpc::Status NoteServiceImpl::ListNotes(grpc::ServerContext*,
                                        const notenest::note::ListNotesRequest* request,
                                        notenest::note::ListNotesResponse* response) {
    auto notes = note_store_.getAllNotes(request->owner_id());
    for (const auto& n : notes) {
        auto* nr = response->add_notes();
        nr->set_id(n.id);
        nr->set_title(n.title);
        nr->set_content(n.content);
        nr->set_owner_id(n.owner_id);
        nr->set_created_at(n.created_at);
        nr->set_updated_at(n.created_at);
    }
    return grpc::Status::OK;
}

grpc::Status NoteServiceImpl::UpdateNote(grpc::ServerContext*,
                                         const notenest::note::UpdateNoteRequest* request,
                                         notenest::note::NoteResponse* response) {
    auto note_opt = note_store_.updateNote(request->note_id(), request->title(), request->content(),
                                           request->requesting_user_id());
    if (!note_opt) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "Note not found or unauthorized");
    }
    response->set_id(note_opt->id);
    response->set_title(note_opt->title);
    response->set_content(note_opt->content);
    response->set_owner_id(note_opt->owner_id);
    response->set_created_at(note_opt->created_at);
    response->set_updated_at(note_opt->created_at);
    return grpc::Status::OK;
}

grpc::Status NoteServiceImpl::DeleteNote(grpc::ServerContext*,
                                         const notenest::note::DeleteNoteRequest* request,
                                         notenest::note::DeleteNoteResponse* response) {
    bool ok = note_store_.deleteNote(request->note_id(), request->requesting_user_id());
    response->set_success(ok);
    if (!ok) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "Note not found or unauthorized");
    }
    return grpc::Status::OK;
}

grpc::Status NoteServiceImpl::ShareNote(grpc::ServerContext*,
                                        const notenest::note::ShareNoteRequest* request,
                                        notenest::note::ShareNoteResponse* response) {
    auto target_user = user_repo_.getUserByEmail(request->target_email());
    if (!target_user) {
        response->set_success(false);
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "Target user email not found");
    }
    bool ok = note_store_.shareNote(request->note_id(), target_user->id, request->owner_id());
    response->set_success(ok);
    response->set_target_user_id(target_user->id);
    if (!ok) {
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "Failed to share note");
    }
    return grpc::Status::OK;
}

grpc::Status NoteServiceImpl::GetSharedNotes(grpc::ServerContext*,
                                             const notenest::note::GetSharedNotesRequest*,
                                             notenest::note::ListNotesResponse*) {
    return grpc::Status::OK;
}

AttachmentServiceImpl::AttachmentServiceImpl(NoteStore& note_store, std::string minio_ext_endpoint)
    : note_store_(note_store), minio_ext_endpoint_(std::move(minio_ext_endpoint)) {}

grpc::Status AttachmentServiceImpl::GenerateUploadUrl(
    grpc::ServerContext*, const notenest::attachment::GenerateUploadUrlRequest* request,
    notenest::attachment::GenerateUploadUrlResponse* response) {
    // Reject unsafe filenames: no path separators, traversal or control chars.
    auto safe_name = Utils::sanitizeFilename(request->filename());
    if (!safe_name) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "Invalid filename: path separators, traversal sequences and control "
                            "characters are not allowed");
    }

    std::string att_id = Utils::generateUUID();
    std::string object_key = "attachments/" + request->note_id() + "/" + att_id + "_" + *safe_name;
    std::string bucket = note_store_.getBucketName();
    ObjectStore* os = note_store_.getObjectStore();
    std::string url;
    if (os) {
        url = os->generatePresignedPutUrl(bucket, object_key, 3600);
    }

    response->set_attachment_id(att_id);
    response->set_key(object_key);
    response->set_upload_url(url);
    return grpc::Status::OK;
}

grpc::Status AttachmentServiceImpl::CompleteUpload(
    grpc::ServerContext*, const notenest::attachment::CompleteUploadRequest* request,
    notenest::attachment::AttachmentResponse* response) {
    long long current_size = note_store_.getTotalAttachmentSizeForUser(request->user_id());
    const long long max_quota = 100 * 1024 * 1024;
    if (current_size + request->size_bytes() > max_quota) {
        return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                            "Attachment storage quota exceeded (100MB max)");
    }

    // Filenames must be safe before they are persisted or used in keys.
    auto safe_name = Utils::sanitizeFilename(request->filename());
    if (!safe_name) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "Invalid filename: path separators, traversal sequences and control "
                            "characters are not allowed");
    }

    // Reject completions with mismatched object keys.
    std::string expected_key =
        "attachments/" + request->note_id() + "/" + request->attachment_id() + "_" + *safe_name;
    if (request->object_key() != expected_key) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Attachment key mismatch");
    }

    std::string bucket = note_store_.getBucketName();
    bool ok = note_store_.addAttachment(request->attachment_id(), request->note_id(), bucket,
                                        request->object_key(), request->size_bytes(),
                                        request->filename(), request->user_id());
    if (!ok) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to save attachment metadata");
    }

    response->set_id(request->attachment_id());
    response->set_note_id(request->note_id());
    response->set_filename(request->filename());
    response->set_object_key(request->object_key());
    response->set_size_bytes(request->size_bytes());
    response->set_download_url(minio_ext_endpoint_ + "/" + bucket + "/" + request->object_key());
    return grpc::Status::OK;
}

grpc::Status AttachmentServiceImpl::DeleteAttachment(
    grpc::ServerContext*, const notenest::attachment::DeleteAttachmentRequest* request,
    notenest::attachment::DeleteAttachmentResponse* response) {
    bool ok = note_store_.deleteAttachment(request->attachment_id(), request->note_id(),
                                           request->user_id());
    response->set_success(ok);
    if (!ok) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "Attachment not found or unauthorized");
    }
    return grpc::Status::OK;
}

grpc::Status AttachmentServiceImpl::ListAttachments(
    grpc::ServerContext*, const notenest::attachment::ListAttachmentsRequest* request,
    notenest::attachment::ListAttachmentsResponse* response) {
    auto note_opt = note_store_.getNoteById(request->note_id(), request->user_id());
    if (!note_opt) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "Note not found or unauthorized");
    }

    std::string bucket = note_store_.getBucketName();
    for (const auto& att : note_opt->attachments) {
        auto* a = response->add_attachments();
        a->set_id(att.id);
        a->set_note_id(att.note_id);
        a->set_filename(att.filename);
        a->set_object_key(att.key);
        a->set_size_bytes(att.size);
        a->set_created_at("");
        a->set_download_url(minio_ext_endpoint_ + "/" + bucket + "/" + att.key);
    }
    return grpc::Status::OK;
}

NotificationServiceImpl::NotificationServiceImpl(EventBus& event_bus) : event_bus_(event_bus) {}

grpc::Status NotificationServiceImpl::Notify(grpc::ServerContext*,
                                             const notenest::note::NotifyRequest* request,
                                             notenest::note::NotifyResponse* response) {
    if (request->user_id().empty() || request->payload().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Missing user_id or payload");
    }
    event_bus_.publish(request->user_id(), request->payload());
    response->set_delivered(true);
    return grpc::Status::OK;
}

GrpcServerRunner::GrpcServerRunner(NoteStore& note_store, UserRepository& user_repo,
                                   EventBus& event_bus, std::string note_port,
                                   std::string attachment_port)
    : note_store_(note_store),
      user_repo_(user_repo),
      event_bus_(event_bus),
      note_port_(std::move(note_port)),
      attachment_port_(std::move(attachment_port)) {}

GrpcServerRunner::~GrpcServerRunner() {
    stop();
}

void GrpcServerRunner::start() {
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();

    // Start Note gRPC Service
    note_service_ = std::make_unique<NoteServiceImpl>(note_store_, user_repo_);
    grpc::ServerBuilder builder_note;
    builder_note.AddListeningPort("0.0.0.0:" + note_port_, grpc::InsecureServerCredentials());
    builder_note.RegisterService(note_service_.get());
    note_server_ = builder_note.BuildAndStart();
    note_thread_ = std::thread([this]() {
        if (note_server_) {
            std::cout << "[gRPC Note Service] Listening on 0.0.0.0:" << note_port_ << std::endl;
            note_server_->Wait();
        }
    });

    // Start Attachment & Notification gRPC Service
    const char* ext_ep = std::getenv("MINIO_EXTERNAL_ENDPOINT");
    std::string minio_ext = ext_ep ? std::string(ext_ep) : "http://localhost:8081";
    attachment_service_ = std::make_unique<AttachmentServiceImpl>(note_store_, minio_ext);
    notification_service_ = std::make_unique<NotificationServiceImpl>(event_bus_);
    grpc::ServerBuilder builder_att;
    builder_att.AddListeningPort("0.0.0.0:" + attachment_port_, grpc::InsecureServerCredentials());
    builder_att.RegisterService(attachment_service_.get());
    builder_att.RegisterService(notification_service_.get());
    attachment_server_ = builder_att.BuildAndStart();
    attachment_thread_ = std::thread([this]() {
        if (attachment_server_) {
            std::cout << "[gRPC Attachment & Notification Service] Listening on 0.0.0.0:"
                      << attachment_port_ << std::endl;
            attachment_server_->Wait();
        }
    });
}

void GrpcServerRunner::stop() {
    auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(3);
    if (note_server_) {
        note_server_->Shutdown(deadline);
        note_server_ = nullptr;
    }
    if (attachment_server_) {
        attachment_server_->Shutdown(deadline);
        attachment_server_ = nullptr;
    }
    if (note_thread_.joinable()) {
        note_thread_.join();
    }
    if (attachment_thread_.joinable()) {
        attachment_thread_.join();
    }
}
