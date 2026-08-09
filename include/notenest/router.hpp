#ifndef ROUTER_HPP
#define ROUTER_HPP

#include <notenest/auth_middleware.hpp>
#include <notenest/auth_service.hpp>
#include <notenest/event_bus.hpp>
#include <notenest/grpc_clients.hpp>
#include <notenest/http_types.hpp>
#include <notenest/note_store.hpp>
#include <notenest/room_registry.hpp>

// Route dispatcher for handling endpoints.
class Router {
public:
    Router(NoteStore& store, AuthService& auth_service, EventBus* event_bus = nullptr,
           RoomRegistry* room_registry = nullptr, AuthGrpcClient* auth_grpc_client = nullptr);

    // Dispatches the HTTP request to the matching handler.
    HttpResponse route(HttpRequest& req, int fd = -1) const;

private:
    NoteStore& store_;
    AuthService& auth_service_;
    AuthGrpcClient* auth_grpc_client_ = nullptr;
    AuthMiddleware auth_middleware_;
    EventBus* event_bus_ = nullptr;
    RoomRegistry* room_registry_ = nullptr;

    HttpResponse handleSignup(const std::string& body) const;
    HttpResponse routeInternal(HttpRequest& req, int fd) const;
    HttpResponse handleLogin(const std::string& body) const;
    HttpResponse handleGetNotes(const std::string& owner_id) const;
    HttpResponse handlePostNotes(const std::string& body, const std::string& owner_id) const;
    HttpResponse handleGetNote(const std::string& id_str, const std::string& owner_id) const;
    HttpResponse handlePutNote(const std::string& id_str, const std::string& body,
                               const std::string& owner_id) const;
    HttpResponse handleDeleteNote(const std::string& id_str, const std::string& owner_id) const;
    HttpResponse handlePostAttachment(const std::string& note_id, const std::string& body,
                                      const std::string& owner_id) const;
    HttpResponse handleCompleteAttachment(const std::string& note_id, const std::string& body,
                                          const std::string& owner_id) const;
    HttpResponse handleDeleteAttachment(const std::string& note_id,
                                        const std::string& attachment_id,
                                        const std::string& owner_id) const;
    HttpResponse handleEvents(HttpRequest& req, int fd) const;
    HttpResponse handleShareNote(const std::string& note_id, const std::string& body,
                                 const std::string& owner_id) const;
    HttpResponse handleGetNoteShares(const std::string& note_id, const std::string& owner_id) const;
    HttpResponse handleDeleteNoteShare(const std::string& note_id,
                                       const std::string& target_user_id,
                                       const std::string& owner_id) const;
    HttpResponse handleExportPdf(const std::string& note_id, const std::string& user_id) const;
    HttpResponse handleNoteWebSocket(const std::string& note_id, HttpRequest& req, int fd) const;
};

#endif
