#include <notenest/db_pool.hpp>
#include <notenest/note_repo.hpp>
#include <print>
#include <unordered_map>

std::vector<Note> PgNoteRepository::getAllNotes(const std::string& owner_id) {
    DBConnectionGuard guard(DBPoolMode::READ);
    PGconn* conn = guard.get();
    if (!conn) {
        return {};
    }

    const char* query =
        "SELECT n.id, n.title, n.content, to_char(n.created_at, 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
        "n.owner_id::text, "
        "CASE WHEN n.owner_id = $1::uuid THEN 'owner' ELSE COALESCE(s.permission, 'editor') END AS "
        "perm "
        "FROM notes n LEFT JOIN note_shares s ON n.id = s.note_id AND s.shared_with_user_id = "
        "$1::uuid "
        "WHERE n.owner_id = $1::uuid OR s.shared_with_user_id = $1::uuid ORDER BY n.id ASC";
    const char* paramValues[1] = {owner_id.c_str()};
    PGresult* res = PQexecParams(conn, query, 1, nullptr, paramValues, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::println(stderr, "DB Error: {}", PQerrorMessage(conn));
        PQclear(res);
        return {};
    }

    std::vector<Note> notes;
    int rows = PQntuples(res);
    for (int i = 0; i < rows; ++i) {
        Note note;
        note.id = PQgetvalue(res, i, 0);
        note.title = PQgetvalue(res, i, 1);
        note.content = PQgetvalue(res, i, 2);
        note.created_at = PQgetvalue(res, i, 3);
        note.owner_id = PQgetvalue(res, i, 4);
        note.permission = PQgetvalue(res, i, 5);
        notes.push_back(note);
    }
    PQclear(res);

    // Single batched fetch; one round-trip instead of one per note.
    if (!notes.empty()) {
        std::string id_list = "{";
        for (size_t i = 0; i < notes.size(); ++i) {
            if (i > 0)
                id_list += ",";
            id_list += "\"" + notes[i].id + "\"";
        }
        id_list += "}";

        const char* att_query =
            "SELECT note_id::text, id::text, bucket, key, size, filename FROM attachments "
            "WHERE note_id = ANY($1::uuid[]) ORDER BY created_at ASC";
        const char* att_params[1] = {id_list.c_str()};
        PGresult* att_res =
            PQexecParams(conn, att_query, 1, nullptr, att_params, nullptr, nullptr, 0);
        if (PQresultStatus(att_res) == PGRES_TUPLES_OK) {
            std::unordered_map<std::string, std::vector<Attachment>> by_note;
            for (int i = 0; i < PQntuples(att_res); ++i) {
                Attachment att;
                std::string note_id = PQgetvalue(att_res, i, 0);
                att.id = PQgetvalue(att_res, i, 1);
                att.note_id = note_id;
                att.bucket = PQgetvalue(att_res, i, 2);
                att.key = PQgetvalue(att_res, i, 3);
                try {
                    att.size = std::stoll(PQgetvalue(att_res, i, 4));
                } catch (...) {
                    att.size = 0;
                }
                att.filename = PQgetvalue(att_res, i, 5);
                att.url = "";  // Populated dynamically by the Store layer
                by_note[note_id].push_back(std::move(att));
            }
            for (auto& note : notes) {
                auto it = by_note.find(note.id);
                if (it != by_note.end()) {
                    note.attachments = std::move(it->second);
                }
            }
        } else {
            std::println(stderr, "DB Error fetching attachments: {}", PQerrorMessage(conn));
        }
        PQclear(att_res);
    }
    return notes;
}

std::optional<Note> PgNoteRepository::getNoteById(const std::string& id,
                                                  const std::string& owner_id) {
    DBConnectionGuard guard(DBPoolMode::READ);
    PGconn* conn = guard.get();
    if (!conn) {
        return std::nullopt;
    }

    const char* query =
        "SELECT n.id, n.title, n.content, to_char(n.created_at, 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
        "n.owner_id::text, "
        "CASE WHEN n.owner_id = $2::uuid THEN 'owner' ELSE COALESCE(s.permission, 'editor') END AS "
        "perm "
        "FROM notes n LEFT JOIN note_shares s ON n.id = s.note_id AND s.shared_with_user_id = "
        "$2::uuid "
        "WHERE n.id = $1::uuid AND (n.owner_id = $2::uuid OR s.shared_with_user_id = $2::uuid)";
    const char* paramValues[2] = {id.c_str(), owner_id.c_str()};
    PGresult* res = PQexecParams(conn, query, 2, nullptr, paramValues, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::println(stderr, "DB Error: {}", PQerrorMessage(conn));
        PQclear(res);
        return std::nullopt;
    }

    if (PQntuples(res) == 0) {
        PQclear(res);
        return std::nullopt;
    }

    Note note;
    note.id = PQgetvalue(res, 0, 0);
    note.title = PQgetvalue(res, 0, 1);
    note.content = PQgetvalue(res, 0, 2);
    note.created_at = PQgetvalue(res, 0, 3);
    note.owner_id = PQgetvalue(res, 0, 4);
    note.permission = PQgetvalue(res, 0, 5);
    PQclear(res);

    note.attachments = getAttachmentsForNote(note.id);
    return note;
}

#include <nlohmann/json.hpp>

Note PgNoteRepository::createNote(const std::string& title, const std::string& content,
                                  const std::string& owner_id) {
    DBConnectionGuard guard(DBPoolMode::WRITE);
    PGconn* conn = guard.get();
    if (!conn) {
        return {};
    }

    PQexec(conn, "BEGIN");
    const char* query =
        "INSERT INTO notes (title, content, owner_id) VALUES ($1, $2, $3) RETURNING id, title, "
        "content, to_char(created_at, 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), owner_id::text";
    const char* paramValues[3] = {title.c_str(), content.c_str(), owner_id.c_str()};
    PGresult* res = PQexecParams(conn, query, 3, nullptr, paramValues, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::println(stderr, "DB Error: {}", PQerrorMessage(conn));
        PQclear(res);
        PQexec(conn, "ROLLBACK");
        return {};
    }

    Note note;
    if (PQntuples(res) > 0) {
        note.id = PQgetvalue(res, 0, 0);
        note.title = PQgetvalue(res, 0, 1);
        note.content = PQgetvalue(res, 0, 2);
        note.created_at = PQgetvalue(res, 0, 3);
        note.owner_id = PQgetvalue(res, 0, 4);
        note.permission = "owner";
    }
    PQclear(res);

    if (!note.id.empty()) {
        nlohmann::json payload = {{"event_type", "note_created"},
                                  {"note_id", note.id},
                                  {"owner_id", note.owner_id},
                                  {"title", note.title},
                                  {"timestamp", note.created_at}};
        std::string payload_str = payload.dump();
        const char* outbox_query =
            "INSERT INTO outbox (event_type, payload) VALUES ('note.events', $1::jsonb)";
        const char* outbox_params[1] = {payload_str.c_str()};
        PGresult* outbox_res =
            PQexecParams(conn, outbox_query, 1, nullptr, outbox_params, nullptr, nullptr, 0);
        PQclear(outbox_res);
    }
    PQexec(conn, "COMMIT");

    if (!note.id.empty()) {
        note.attachments = {};
    }
    return note;
}

std::optional<Note> PgNoteRepository::updateNote(const std::string& id, const std::string& title,
                                                 const std::string& content,
                                                 const std::string& owner_id) {
    DBConnectionGuard guard(DBPoolMode::WRITE);
    PGconn* conn = guard.get();
    if (!conn) {
        return std::nullopt;
    }

    PQexec(conn, "BEGIN");
    const char* query =
        "UPDATE notes SET title = $2, content = $3 "
        "WHERE id = $1::uuid AND ("
        "  owner_id = $4::uuid OR EXISTS ("
        "    SELECT 1 FROM note_shares "
        "    WHERE note_id = $1::uuid AND shared_with_user_id = $4::uuid AND permission = 'editor'"
        "  )"
        ") RETURNING id, title, content, to_char(created_at, 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
        "owner_id::text";
    const char* paramValues[4] = {id.c_str(), title.c_str(), content.c_str(), owner_id.c_str()};
    PGresult* res = PQexecParams(conn, query, 4, nullptr, paramValues, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::println(stderr, "DB Error: {}", PQerrorMessage(conn));
        PQclear(res);
        PQexec(conn, "ROLLBACK");
        return std::nullopt;
    }

    if (PQntuples(res) == 0) {
        PQclear(res);
        PQexec(conn, "ROLLBACK");
        return std::nullopt;
    }

    Note note;
    note.id = PQgetvalue(res, 0, 0);
    note.title = PQgetvalue(res, 0, 1);
    note.content = PQgetvalue(res, 0, 2);
    note.created_at = PQgetvalue(res, 0, 3);
    note.owner_id = PQgetvalue(res, 0, 4);
    note.permission = (note.owner_id == owner_id) ? "owner" : "editor";
    PQclear(res);

    nlohmann::json payload = {{"event_type", "note_updated"},
                              {"note_id", note.id},
                              {"owner_id", note.owner_id},
                              {"title", note.title},
                              {"timestamp", note.created_at}};
    std::string payload_str = payload.dump();
    const char* outbox_query =
        "INSERT INTO outbox (event_type, payload) VALUES ('note.events', $1::jsonb)";
    const char* outbox_params[1] = {payload_str.c_str()};
    PGresult* outbox_res =
        PQexecParams(conn, outbox_query, 1, nullptr, outbox_params, nullptr, nullptr, 0);
    PQclear(outbox_res);

    PQexec(conn, "COMMIT");

    note.attachments = getAttachmentsForNote(note.id);
    return note;
}

bool PgNoteRepository::deleteNote(const std::string& id, const std::string& owner_id) {
    DBConnectionGuard guard(DBPoolMode::WRITE);
    PGconn* conn = guard.get();
    if (!conn) {
        return false;
    }

    PQexec(conn, "BEGIN");
    const char* query = "DELETE FROM notes WHERE id = $1 AND owner_id = $2";
    const char* paramValues[2] = {id.c_str(), owner_id.c_str()};
    PGresult* res = PQexecParams(conn, query, 2, nullptr, paramValues, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::println(stderr, "DB Error: {}", PQerrorMessage(conn));
        PQclear(res);
        PQexec(conn, "ROLLBACK");
        return false;
    }

    int affected = 0;
    try {
        affected = std::stoi(PQcmdTuples(res));
    } catch (...) {
        affected = 0;
    }
    PQclear(res);

    if (affected > 0) {
        nlohmann::json payload = {
            {"event_type", "note_deleted"}, {"note_id", id}, {"owner_id", owner_id}};
        std::string payload_str = payload.dump();
        const char* outbox_query =
            "INSERT INTO outbox (event_type, payload) VALUES ('note.events', $1::jsonb)";
        const char* outbox_params[1] = {payload_str.c_str()};
        PGresult* outbox_res =
            PQexecParams(conn, outbox_query, 1, nullptr, outbox_params, nullptr, nullptr, 0);
        PQclear(outbox_res);
    }
    PQexec(conn, "COMMIT");

    return affected > 0;
}

std::vector<Attachment> PgNoteRepository::getAttachmentsForNote(const std::string& note_id) {
    DBConnectionGuard guard(DBPoolMode::READ);
    PGconn* conn = guard.get();
    if (!conn) {
        return {};
    }

    const char* query =
        "SELECT id, note_id, bucket, key, size, filename FROM attachments WHERE note_id = $1 ORDER "
        "BY created_at ASC";
    const char* paramValues[1] = {note_id.c_str()};
    PGresult* res = PQexecParams(conn, query, 1, nullptr, paramValues, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::println(stderr, "DB Error fetching attachments: {}", PQerrorMessage(conn));
        PQclear(res);
        return {};
    }

    std::vector<Attachment> attachments;
    int rows = PQntuples(res);
    for (int i = 0; i < rows; ++i) {
        Attachment att;
        att.id = PQgetvalue(res, i, 0);
        att.note_id = PQgetvalue(res, i, 1);
        att.bucket = PQgetvalue(res, i, 2);
        att.key = PQgetvalue(res, i, 3);
        try {
            att.size = std::stoll(PQgetvalue(res, i, 4));
        } catch (...) {
            att.size = 0;
        }
        att.filename = PQgetvalue(res, i, 5);
        att.url = "";  // Will be populated dynamically by the Store layer
        attachments.push_back(att);
    }
    PQclear(res);
    return attachments;
}

bool PgNoteRepository::addAttachment(const std::string& id, const std::string& note_id,
                                     const std::string& bucket, const std::string& key,
                                     long long size, const std::string& filename) {
    DBConnectionGuard guard(DBPoolMode::WRITE);
    PGconn* conn = guard.get();
    if (!conn) {
        return false;
    }

    const char* query =
        "INSERT INTO attachments (id, note_id, bucket, key, size, filename) VALUES ($1, $2, $3, "
        "$4, $5, $6)";
    std::string size_str = std::to_string(size);
    const char* paramValues[6] = {id.c_str(),  note_id.c_str(),  bucket.c_str(),
                                  key.c_str(), size_str.c_str(), filename.c_str()};
    PGresult* res = PQexecParams(conn, query, 6, nullptr, paramValues, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::println(stderr, "DB Error adding attachment: {}", PQerrorMessage(conn));
        PQclear(res);
        return false;
    }
    PQclear(res);
    return true;
}

std::optional<std::pair<std::string, std::string>> PgNoteRepository::deleteAttachment(
    const std::string& attachment_id, const std::string& note_id, const std::string& owner_id) {
    DBConnectionGuard guard(DBPoolMode::WRITE);
    PGconn* conn = guard.get();
    if (!conn) {
        return std::nullopt;
    }

    const char* query =
        "DELETE FROM attachments USING notes WHERE attachments.note_id = notes.id AND "
        "attachments.id = $1 AND attachments.note_id = $2 AND notes.owner_id = $3 "
        "RETURNING attachments.bucket, attachments.key";
    const char* paramValues[3] = {attachment_id.c_str(), note_id.c_str(), owner_id.c_str()};
    PGresult* res = PQexecParams(conn, query, 3, nullptr, paramValues, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        PQclear(res);
        return std::nullopt;
    }

    std::string bucket = PQgetvalue(res, 0, 0);
    std::string key = PQgetvalue(res, 0, 1);
    PQclear(res);
    return std::make_pair(bucket, key);
}

long long PgNoteRepository::getTotalAttachmentSizeForUser(const std::string& owner_id) {
    DBConnectionGuard guard(DBPoolMode::READ);
    PGconn* conn = guard.get();
    if (!conn) {
        return 0;
    }

    const char* query =
        "SELECT COALESCE(SUM(attachments.size), 0) FROM attachments JOIN notes ON "
        "attachments.note_id = notes.id WHERE notes.owner_id = $1";
    const char* paramValues[1] = {owner_id.c_str()};
    PGresult* res = PQexecParams(conn, query, 1, nullptr, paramValues, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        PQclear(res);
        return 0;
    }

    long long total = 0;
    try {
        total = std::stoll(PQgetvalue(res, 0, 0));
    } catch (...) {
        total = 0;
    }
    PQclear(res);
    return total;
}

bool PgNoteRepository::shareNote(const std::string& note_id, const std::string& shared_with_user_id,
                                 const std::string& permission) {
    DBConnectionGuard guard(DBPoolMode::WRITE);
    PGconn* conn = guard.get();
    if (!conn) {
        return false;
    }

    std::string perm = (permission == "viewer") ? "viewer" : "editor";
    PQexec(conn, "BEGIN");
    const char* query =
        "INSERT INTO note_shares (note_id, shared_with_user_id, permission) VALUES ($1::uuid, "
        "$2::uuid, $3) "
        "ON CONFLICT (note_id, shared_with_user_id) DO UPDATE SET permission = EXCLUDED.permission";
    const char* paramValues[3] = {note_id.c_str(), shared_with_user_id.c_str(), perm.c_str()};
    PGresult* res = PQexecParams(conn, query, 3, nullptr, paramValues, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::println(stderr, "DB Error sharing note: {}", PQerrorMessage(conn));
        PQclear(res);
        PQexec(conn, "ROLLBACK");
        return false;
    }
    PQclear(res);

    nlohmann::json payload = {{"event_type", "note_shared"},
                              {"note_id", note_id},
                              {"shared_with_user_id", shared_with_user_id},
                              {"permission", perm}};
    std::string payload_str = payload.dump();
    const char* outbox_query =
        "INSERT INTO outbox (event_type, payload) VALUES ('note.events', $1::jsonb)";
    const char* outbox_params[1] = {payload_str.c_str()};
    PGresult* outbox_res =
        PQexecParams(conn, outbox_query, 1, nullptr, outbox_params, nullptr, nullptr, 0);
    PQclear(outbox_res);
    PQexec(conn, "COMMIT");

    return true;
}

std::vector<NoteShareDetail> PgNoteRepository::getNoteShares(const std::string& note_id,
                                                             const std::string& owner_id) {
    std::vector<NoteShareDetail> details;
    DBConnectionGuard guard(DBPoolMode::READ);
    PGconn* conn = guard.get();
    if (!conn) {
        return details;
    }

    const char* query =
        "SELECT s.shared_with_user_id::text, u.email, COALESCE(s.permission, 'editor') "
        "FROM note_shares s JOIN users u ON s.shared_with_user_id = u.id "
        "JOIN notes n ON s.note_id = n.id "
        "WHERE s.note_id = $1::uuid AND n.owner_id = $2::uuid ORDER BY s.created_at ASC";
    const char* paramValues[2] = {note_id.c_str(), owner_id.c_str()};
    PGresult* res = PQexecParams(conn, query, 2, nullptr, paramValues, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        return details;
    }

    int rows = PQntuples(res);
    for (int i = 0; i < rows; ++i) {
        NoteShareDetail detail;
        detail.user_id = PQgetvalue(res, i, 0);
        detail.email = PQgetvalue(res, i, 1);
        detail.permission = PQgetvalue(res, i, 2);
        details.push_back(detail);
    }
    PQclear(res);
    return details;
}

bool PgNoteRepository::removeNoteShare(const std::string& note_id,
                                       const std::string& shared_with_user_id,
                                       const std::string& owner_id) {
    DBConnectionGuard guard(DBPoolMode::WRITE);
    PGconn* conn = guard.get();
    if (!conn) {
        return false;
    }

    const char* query =
        "DELETE FROM note_shares WHERE note_id = $1::uuid AND shared_with_user_id = $2::uuid "
        "AND EXISTS (SELECT 1 FROM notes WHERE id = $1::uuid AND owner_id = $3::uuid)";
    const char* paramValues[3] = {note_id.c_str(), shared_with_user_id.c_str(), owner_id.c_str()};
    PGresult* res = PQexecParams(conn, query, 3, nullptr, paramValues, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        return false;
    }
    PQclear(res);
    return true;
}

std::vector<std::string> PgNoteRepository::getShareRecipients(const std::string& note_id) {
    std::vector<std::string> recipients;
    DBConnectionGuard guard(DBPoolMode::READ);
    PGconn* conn = guard.get();
    if (!conn) {
        return recipients;
    }

    const char* query =
        "SELECT shared_with_user_id::text FROM note_shares WHERE note_id = $1::uuid";
    const char* paramValues[1] = {note_id.c_str()};
    PGresult* res = PQexecParams(conn, query, 1, nullptr, paramValues, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        return recipients;
    }

    int rows = PQntuples(res);
    for (int i = 0; i < rows; ++i) {
        recipients.push_back(PQgetvalue(res, i, 0));
    }
    PQclear(res);
    return recipients;
}

std::string PgNoteRepository::getUserPermissionForNote(const std::string& note_id,
                                                       const std::string& user_id) {
    DBConnectionGuard guard(DBPoolMode::READ);
    PGconn* conn = guard.get();
    if (!conn) {
        return "";
    }

    const char* query =
        "SELECT CASE WHEN n.owner_id = $2::uuid THEN 'owner' ELSE COALESCE(s.permission, 'editor') "
        "END "
        "FROM notes n LEFT JOIN note_shares s ON n.id = s.note_id AND s.shared_with_user_id = "
        "$2::uuid "
        "WHERE n.id = $1::uuid AND (n.owner_id = $2::uuid OR s.shared_with_user_id = $2::uuid)";
    const char* paramValues[2] = {note_id.c_str(), user_id.c_str()};
    PGresult* res = PQexecParams(conn, query, 2, nullptr, paramValues, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        PQclear(res);
        return "";
    }

    std::string perm = PQgetvalue(res, 0, 0);
    PQclear(res);
    return perm;
}
