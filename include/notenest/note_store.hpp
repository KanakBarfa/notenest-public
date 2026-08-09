#ifndef NOTE_STORE_HPP
#define NOTE_STORE_HPP

#include <memory>
#include <mutex>
#include <notenest/cache.hpp>
#include <notenest/note_repo.hpp>
#include <notenest/object_store.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Service layer mapping store requests to database repository.
class NoteStore {
public:
    explicit NoteStore(NoteRepository& repo, Cache* cache = nullptr,
                       ObjectStore* object_store = nullptr, std::string bucket_name = "");

    // Returns all stored notes for a user from database.
    std::vector<Note> getAllNotes(const std::string& owner_id) const;

    // Returns a note by UUID for a user if it exists.
    std::optional<Note> getNoteById(const std::string& id, const std::string& owner_id) const;

    // Creates and returns a new note for a user in database.
    Note createNote(const std::string& title, const std::string& content,
                    const std::string& owner_id);

    // Updates an existing note for a user and returns it.
    std::optional<Note> updateNote(const std::string& id, const std::string& title,
                                   const std::string& content, const std::string& owner_id);

    // Deletes a note by UUID for a user. Returns true if deleted.
    bool deleteNote(const std::string& id, const std::string& owner_id);

    // Adds a file attachment to an existing note if the user owns it.
    bool addAttachment(const std::string& id, const std::string& note_id, const std::string& bucket,
                       const std::string& key, long long size, const std::string& filename,
                       const std::string& owner_id);

    // Deletes an attachment from note and object storage if owned.
    bool deleteAttachment(const std::string& attachment_id, const std::string& note_id,
                          const std::string& owner_id);

    // Shares a note with another user with permission (viewer or editor).
    bool shareNote(const std::string& note_id, const std::string& target_user_id,
                   const std::string& owner_id, const std::string& permission = "editor");

    // Returns active share details for a note if owned.
    std::vector<NoteShareDetail> getNoteShares(const std::string& note_id,
                                               const std::string& owner_id) const {
        return repo_.getNoteShares(note_id, owner_id);
    }

    // Revokes note access for a recipient.
    bool removeNoteShare(const std::string& note_id, const std::string& target_user_id,
                         const std::string& owner_id);

    // Returns user permission for a note ("owner", "editor", "viewer", or "").
    std::string getUserPermissionForNote(const std::string& note_id,
                                         const std::string& user_id) const {
        return repo_.getUserPermissionForNote(note_id, user_id);
    }

    // Returns total byte size of all attachments owned by user.
    long long getTotalAttachmentSizeForUser(const std::string& owner_id) const {
        return repo_.getTotalAttachmentSizeForUser(owner_id);
    }

    std::string getBucketName() const {
        return bucket_name_;
    }
    ObjectStore* getObjectStore() const {
        return object_store_;
    }

private:
    NoteRepository& repo_;
    Cache* cache_;
    ObjectStore* object_store_;
    std::string bucket_name_;

    mutable std::mutex stampede_map_mutex_;
    mutable std::unordered_map<std::string, std::shared_ptr<std::mutex>> key_mutexes_;

    // Gets or creates a mutex for a specific cache key.
    std::shared_ptr<std::mutex> getMutexForKey(const std::string& key) const;
};

#endif
