#include <nlohmann/json.hpp>
#include <notenest/note_store.hpp>
#include <print>

NoteStore::NoteStore(NoteRepository& repo, Cache* cache, ObjectStore* object_store,
                     std::string bucket_name)
    : repo_(repo), cache_(cache), object_store_(object_store), bucket_name_(bucket_name) {}

std::shared_ptr<std::mutex> NoteStore::getMutexForKey(const std::string& key) const {
    std::lock_guard<std::mutex> lock(stampede_map_mutex_);
    auto& m = key_mutexes_[key];
    if (!m) {
        m = std::make_shared<std::mutex>();
    }
    return m;
}

std::vector<Note> NoteStore::getAllNotes(const std::string& owner_id) const {
    std::string key = "user:" + owner_id + ":notes";
    if (cache_) {
        auto cached = cache_->get(key);
        if (cached) {
            try {
                auto res = nlohmann::json::parse(*cached).get<std::vector<Note>>();
                std::println("Cache HIT for key: {}", key);
                if (object_store_) {
                    for (auto& note : res) {
                        for (auto& att : note.attachments) {
                            att.url =
                                object_store_->generatePresignedGetUrl(att.bucket, att.key, 3600);
                        }
                    }
                }
                return res;
            } catch (...) {
                // Ignore parsing errors and fall back to DB
            }
        }
        std::println("Cache MISS for key: {}", key);
    }

    auto key_mutex = getMutexForKey(key);
    std::lock_guard<std::mutex> lock(*key_mutex);

    // Double-checked locking
    if (cache_) {
        auto cached = cache_->get(key);
        if (cached) {
            try {
                auto res = nlohmann::json::parse(*cached).get<std::vector<Note>>();
                std::println("Cache HIT (double-checked) for key: {}", key);
                if (object_store_) {
                    for (auto& note : res) {
                        for (auto& att : note.attachments) {
                            att.url =
                                object_store_->generatePresignedGetUrl(att.bucket, att.key, 3600);
                        }
                    }
                }
                return res;
            } catch (...) {
                // Ignore parsing errors
            }
        }
    }

    auto notes = repo_.getAllNotes(owner_id);
    if (cache_) {
        std::println("Cache SET for key: {}", key);
        auto notes_copy = notes;
        for (auto& n : notes_copy) {
            for (auto& att : n.attachments) {
                att.url = "";
            }
        }
        cache_->set(key, nlohmann::json(notes_copy).dump(), 300);
    }

    if (object_store_) {
        for (auto& note : notes) {
            for (auto& att : note.attachments) {
                att.url = object_store_->generatePresignedGetUrl(att.bucket, att.key, 3600);
            }
        }
    }

    return notes;
}

std::optional<Note> NoteStore::getNoteById(const std::string& id,
                                           const std::string& owner_id) const {
    std::string key = "note:" + id;
    if (cache_) {
        auto cached = cache_->get(key);
        if (cached) {
            try {
                auto res = nlohmann::json::parse(*cached).get<Note>();
                std::println("Cache HIT for key: {}", key);
                if (res.owner_id != owner_id) {
                    std::string perm = repo_.getUserPermissionForNote(id, owner_id);
                    if (perm.empty()) {
                        return std::nullopt;
                    }
                    res.permission = perm;
                } else {
                    res.permission = "owner";
                }
                if (object_store_) {
                    for (auto& att : res.attachments) {
                        att.url = object_store_->generatePresignedGetUrl(att.bucket, att.key, 3600);
                    }
                }
                return res;
            } catch (...) {
                // Ignore parsing errors and fall back to DB
            }
        }
        std::println("Cache MISS for key: {}", key);
    }

    auto key_mutex = getMutexForKey(key);
    std::lock_guard<std::mutex> lock(*key_mutex);

    // Double-checked locking
    if (cache_) {
        auto cached = cache_->get(key);
        if (cached) {
            try {
                auto res = nlohmann::json::parse(*cached).get<Note>();
                std::println("Cache HIT (double-checked) for key: {}", key);
                if (res.owner_id != owner_id) {
                    std::string perm = repo_.getUserPermissionForNote(id, owner_id);
                    if (perm.empty()) {
                        return std::nullopt;
                    }
                    res.permission = perm;
                } else {
                    res.permission = "owner";
                }
                if (object_store_) {
                    for (auto& att : res.attachments) {
                        att.url = object_store_->generatePresignedGetUrl(att.bucket, att.key, 3600);
                    }
                }
                return res;
            } catch (...) {
                // Ignore parsing errors
            }
        }
    }

    auto note = repo_.getNoteById(id, owner_id);
    if (cache_ && note) {
        std::println("Cache SET for key: {}", key);
        auto note_copy = *note;
        for (auto& att : note_copy.attachments) {
            att.url = "";
        }
        cache_->set(key, nlohmann::json(note_copy).dump(), 300);
    }

    if (object_store_ && note) {
        for (auto& att : note->attachments) {
            att.url = object_store_->generatePresignedGetUrl(att.bucket, att.key, 3600);
        }
    }

    return note;
}

Note NoteStore::createNote(const std::string& title, const std::string& content,
                           const std::string& owner_id) {
    Note note = repo_.createNote(title, content, owner_id);
    if (cache_) {
        std::string key = "user:" + owner_id + ":notes";
        std::println("Cache DEL for key: {}", key);
        cache_->del(key);
    }

    if (object_store_) {
        for (auto& att : note.attachments) {
            att.url = object_store_->generatePresignedGetUrl(att.bucket, att.key, 3600);
        }
    }

    return note;
}

std::optional<Note> NoteStore::updateNote(const std::string& id, const std::string& title,
                                          const std::string& content, const std::string& owner_id) {
    auto recipients = repo_.getShareRecipients(id);
    auto note = repo_.updateNote(id, title, content, owner_id);
    if (cache_) {
        std::string key1 = "note:" + id;
        std::string key2 = "user:" + owner_id + ":notes";
        std::println("Cache DEL for key: {}", key1);
        std::println("Cache DEL for key: {}", key2);
        cache_->del(key1);
        cache_->del(key2);
        for (const auto& r_id : recipients) {
            std::string r_key = "user:" + r_id + ":notes";
            std::println("Cache DEL for shared recipient key: {}", r_key);
            cache_->del(r_key);
        }
    }

    if (object_store_ && note) {
        for (auto& att : note->attachments) {
            att.url = object_store_->generatePresignedGetUrl(att.bucket, att.key, 3600);
        }
    }

    return note;
}

bool NoteStore::deleteNote(const std::string& id, const std::string& owner_id) {
    auto recipients = repo_.getShareRecipients(id);
    auto attachments = repo_.getAttachmentsForNote(id);
    bool deleted = repo_.deleteNote(id, owner_id);
    if (deleted) {
        if (object_store_) {
            for (const auto& att : attachments) {
                object_store_->deleteObject(att.bucket, att.key);
            }
        }
        if (cache_) {
            std::string key1 = "note:" + id;
            std::string key2 = "user:" + owner_id + ":notes";
            std::println("Cache DEL for key: {}", key1);
            std::println("Cache DEL for key: {}", key2);
            cache_->del(key1);
            cache_->del(key2);
            for (const auto& r_id : recipients) {
                std::string r_key = "user:" + r_id + ":notes";
                std::println("Cache DEL for shared recipient key: {}", r_key);
                cache_->del(r_key);
            }
        }
    }
    return deleted;
}

bool NoteStore::addAttachment(const std::string& id, const std::string& note_id,
                              const std::string& bucket, const std::string& key, long long size,
                              const std::string& filename, const std::string& owner_id) {
    auto note_opt = repo_.getNoteById(note_id, owner_id);
    if (!note_opt) {
        return false;
    }

    bool success = repo_.addAttachment(id, note_id, bucket, key, size, filename);
    if (success && cache_) {
        std::string key1 = "note:" + note_id;
        std::string key2 = "user:" + owner_id + ":notes";
        std::println("Cache DEL for key: {}", key1);
        std::println("Cache DEL for key: {}", key2);
        cache_->del(key1);
        cache_->del(key2);
        auto recipients = repo_.getShareRecipients(note_id);
        for (const auto& r_id : recipients) {
            std::string r_key = "user:" + r_id + ":notes";
            std::println("Cache DEL for shared recipient key: {}", r_key);
            cache_->del(r_key);
        }
    }
    return success;
}

bool NoteStore::deleteAttachment(const std::string& attachment_id, const std::string& note_id,
                                 const std::string& owner_id) {
    auto deleted_info = repo_.deleteAttachment(attachment_id, note_id, owner_id);
    if (!deleted_info) {
        return false;
    }

    if (object_store_) {
        object_store_->deleteObject(deleted_info->first, deleted_info->second);
    }

    if (cache_) {
        std::string key1 = "note:" + note_id;
        std::string key2 = "user:" + owner_id + ":notes";
        std::println("Cache DEL for key: {}", key1);
        std::println("Cache DEL for key: {}", key2);
        cache_->del(key1);
        cache_->del(key2);
        auto recipients = repo_.getShareRecipients(note_id);
        for (const auto& r_id : recipients) {
            std::string r_key = "user:" + r_id + ":notes";
            std::println("Cache DEL for shared recipient key: {}", r_key);
            cache_->del(r_key);
        }
    }

    return true;
}

bool NoteStore::shareNote(const std::string& note_id, const std::string& target_user_id,
                          const std::string& owner_id, const std::string& permission) {
    auto note_opt = repo_.getNoteById(note_id, owner_id);
    if (!note_opt) {
        return false;
    }

    bool success = repo_.shareNote(note_id, target_user_id, permission);
    if (success && cache_) {
        std::string key = "user:" + target_user_id + ":notes";
        std::println("Cache DEL for key: {}", key);
        cache_->del(key);
    }
    return success;
}

bool NoteStore::removeNoteShare(const std::string& note_id, const std::string& target_user_id,
                                const std::string& owner_id) {
    bool success = repo_.removeNoteShare(note_id, target_user_id, owner_id);
    if (success && cache_) {
        std::string key = "user:" + target_user_id + ":notes";
        std::println("Cache DEL for key: {}", key);
        cache_->del(key);
    }
    return success;
}
