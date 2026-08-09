#ifndef ATTACHMENT_HPP
#define ATTACHMENT_HPP

#include <nlohmann/json.hpp>
#include <string>

// Represents a file attachment associated with a note.
struct Attachment {
    std::string id;
    std::string note_id;
    std::string bucket;
    std::string key;
    long long size;
    std::string filename;
    std::string url;  // Presigned GET URL generated dynamically
};

// Configures nlohmann/json serialization for Attachment.
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Attachment, id, note_id, bucket, key, size, filename, url)

#endif
