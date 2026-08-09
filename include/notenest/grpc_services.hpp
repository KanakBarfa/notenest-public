#ifndef GRPC_SERVICES_HPP
#define GRPC_SERVICES_HPP

#include <grpcpp/grpcpp.h>

#include <memory>
#include <notenest/event_bus.hpp>
#include <notenest/note_store.hpp>
#include <notenest/user_repo.hpp>
#include <string>
#include <thread>

#include "attachment.grpc.pb.h"
#include "auth.grpc.pb.h"
#include "note.grpc.pb.h"
#include "user.grpc.pb.h"

// Implementation of NoteService gRPC interface.
class NoteServiceImpl final : public notenest::note::NoteService::Service {
public:
    explicit NoteServiceImpl(NoteStore& note_store, UserRepository& user_repo);

    grpc::Status CreateNote(grpc::ServerContext* context,
                            const notenest::note::CreateNoteRequest* request,
                            notenest::note::NoteResponse* response) override;

    grpc::Status GetNote(grpc::ServerContext* context,
                         const notenest::note::GetNoteRequest* request,
                         notenest::note::NoteResponse* response) override;

    grpc::Status ListNotes(grpc::ServerContext* context,
                           const notenest::note::ListNotesRequest* request,
                           notenest::note::ListNotesResponse* response) override;

    grpc::Status UpdateNote(grpc::ServerContext* context,
                            const notenest::note::UpdateNoteRequest* request,
                            notenest::note::NoteResponse* response) override;

    grpc::Status DeleteNote(grpc::ServerContext* context,
                            const notenest::note::DeleteNoteRequest* request,
                            notenest::note::DeleteNoteResponse* response) override;

    grpc::Status ShareNote(grpc::ServerContext* context,
                           const notenest::note::ShareNoteRequest* request,
                           notenest::note::ShareNoteResponse* response) override;

    grpc::Status GetSharedNotes(grpc::ServerContext* context,
                                const notenest::note::GetSharedNotesRequest* request,
                                notenest::note::ListNotesResponse* response) override;

private:
    NoteStore& note_store_;
    UserRepository& user_repo_;
};

// Implementation of AttachmentService gRPC interface.
class AttachmentServiceImpl final : public notenest::attachment::AttachmentService::Service {
public:
    explicit AttachmentServiceImpl(NoteStore& note_store, std::string minio_ext_endpoint);

    grpc::Status GenerateUploadUrl(
        grpc::ServerContext* context, const notenest::attachment::GenerateUploadUrlRequest* request,
        notenest::attachment::GenerateUploadUrlResponse* response) override;

    grpc::Status CompleteUpload(grpc::ServerContext* context,
                                const notenest::attachment::CompleteUploadRequest* request,
                                notenest::attachment::AttachmentResponse* response) override;

    grpc::Status DeleteAttachment(
        grpc::ServerContext* context, const notenest::attachment::DeleteAttachmentRequest* request,
        notenest::attachment::DeleteAttachmentResponse* response) override;

    grpc::Status ListAttachments(grpc::ServerContext* context,
                                 const notenest::attachment::ListAttachmentsRequest* request,
                                 notenest::attachment::ListAttachmentsResponse* response) override;

private:
    NoteStore& note_store_;
    std::string minio_ext_endpoint_;
};

// Implementation of NotificationService gRPC interface.
class NotificationServiceImpl final : public notenest::note::NotificationService::Service {
public:
    explicit NotificationServiceImpl(EventBus& event_bus);

    grpc::Status Notify(grpc::ServerContext* context, const notenest::note::NotifyRequest* request,
                        notenest::note::NotifyResponse* response) override;

private:
    EventBus& event_bus_;
};

// Manager for running gRPC server background instances.
class GrpcServerRunner {
public:
    GrpcServerRunner(NoteStore& note_store, UserRepository& user_repo, EventBus& event_bus,
                     std::string note_port = "50053", std::string attachment_port = "50054");
    ~GrpcServerRunner();

    void start();
    void stop();

private:
    NoteStore& note_store_;
    UserRepository& user_repo_;
    EventBus& event_bus_;
    std::string note_port_;
    std::string attachment_port_;

    std::unique_ptr<grpc::Server> note_server_;
    std::unique_ptr<grpc::Server> attachment_server_;
    std::unique_ptr<NoteServiceImpl> note_service_;
    std::unique_ptr<AttachmentServiceImpl> attachment_service_;
    std::unique_ptr<NotificationServiceImpl> notification_service_;
    std::thread note_thread_;
    std::thread attachment_thread_;
};

#endif
