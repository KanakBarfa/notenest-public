#ifndef NOTE_REPO_HPP
#define NOTE_REPO_HPP

#include <notenest/note.hpp>
#include <optional>
#include <vector>

// Abstract repository interface for note persistence.
class NoteRepository {
public:
    virtual ~NoteRepository() = default;

    virtual std::vector<Note> getAllNotes(const std::string& owner_id) = 0;
    virtual std::optional<Note> getNoteById(const std::string& id, const std::string& owner_id) = 0;
    virtual Note createNote(const std::string& title, const std::string& content,
                            const std::string& owner_id) = 0;
    virtual std::optional<Note> updateNote(const std::string& id, const std::string& title,
                                           const std::string& content,
                                           const std::string& owner_id) = 0;
    virtual bool deleteNote(const std::string& id, const std::string& owner_id) = 0;

    virtual std::vector<Attachment> getAttachmentsForNote(const std::string& note_id) = 0;
    virtual bool addAttachment(const std::string& id, const std::string& note_id,
                               const std::string& bucket, const std::string& key, long long size,
                               const std::string& filename) = 0;
    virtual std::optional<std::pair<std::string, std::string>> deleteAttachment(
        const std::string& attachment_id, const std::string& note_id,
        const std::string& owner_id) = 0;
    virtual bool shareNote(const std::string& note_id, const std::string& shared_with_user_id,
                           const std::string& permission = "editor") = 0;
    virtual std::vector<NoteShareDetail> getNoteShares(const std::string& note_id,
                                                       const std::string& owner_id) = 0;
    virtual bool removeNoteShare(const std::string& note_id, const std::string& shared_with_user_id,
                                 const std::string& owner_id) = 0;
    virtual std::vector<std::string> getShareRecipients(const std::string& note_id) = 0;
    virtual std::string getUserPermissionForNote(const std::string& note_id,
                                                 const std::string& user_id) = 0;
    virtual long long getTotalAttachmentSizeForUser(const std::string& owner_id) = 0;
};

// PostgreSQL-backed note repository implementation.
class PgNoteRepository : public NoteRepository {
public:
    PgNoteRepository() = default;

    std::vector<Note> getAllNotes(const std::string& owner_id) override;
    std::optional<Note> getNoteById(const std::string& id, const std::string& owner_id) override;
    Note createNote(const std::string& title, const std::string& content,
                    const std::string& owner_id) override;
    std::optional<Note> updateNote(const std::string& id, const std::string& title,
                                   const std::string& content,
                                   const std::string& owner_id) override;
    bool deleteNote(const std::string& id, const std::string& owner_id) override;

    std::vector<Attachment> getAttachmentsForNote(const std::string& note_id) override;
    bool addAttachment(const std::string& id, const std::string& note_id, const std::string& bucket,
                       const std::string& key, long long size,
                       const std::string& filename) override;
    std::optional<std::pair<std::string, std::string>> deleteAttachment(
        const std::string& attachment_id, const std::string& note_id,
        const std::string& owner_id) override;
    bool shareNote(const std::string& note_id, const std::string& shared_with_user_id,
                   const std::string& permission = "editor") override;
    std::vector<NoteShareDetail> getNoteShares(const std::string& note_id,
                                               const std::string& owner_id) override;
    bool removeNoteShare(const std::string& note_id, const std::string& shared_with_user_id,
                         const std::string& owner_id) override;
    std::vector<std::string> getShareRecipients(const std::string& note_id) override;
    std::string getUserPermissionForNote(const std::string& note_id,
                                         const std::string& user_id) override;
    long long getTotalAttachmentSizeForUser(const std::string& owner_id) override;
};

#endif
