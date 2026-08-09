#ifndef NOTE_HPP
#define NOTE_HPP

#include <nlohmann/json.hpp>
#include <notenest/attachment.hpp>
#include <string>
#include <vector>

struct NoteShareDetail {
    std::string user_id;
    std::string email;
    std::string permission;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(NoteShareDetail, user_id, email, permission)

// Represents a note entity.
struct Note {
    std::string id;
    std::string title;
    std::string content;
    std::string created_at;
    std::string owner_id;
    std::string permission = "owner";
    std::vector<Attachment> attachments;
};

// Configures nlohmann/json serialization and deserialization for Note.
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Note, id, title, content, created_at, owner_id,
                                                permission, attachments)

#endif
