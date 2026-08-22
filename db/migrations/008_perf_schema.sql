-- 008: Performance & correctness schema updates.
-- Hot-path indexes, TIMESTAMPTZ normalization, chronological ordering.

CREATE INDEX IF NOT EXISTS idx_notes_owner_id ON notes (owner_id);
CREATE INDEX IF NOT EXISTS idx_notes_owner_created_at ON notes (owner_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_note_shares_shared_with_user_id ON note_shares (shared_with_user_id);
CREATE INDEX IF NOT EXISTS idx_attachments_note_id ON attachments (note_id);
CREATE INDEX IF NOT EXISTS idx_comments_note_id_author_id ON comments (note_id, author_id);

-- Normalize naive TIMESTAMP columns to TIMESTAMPTZ (stored values are UTC).
ALTER TABLE notes ALTER COLUMN created_at TYPE TIMESTAMPTZ
    USING created_at AT TIME ZONE 'UTC';
ALTER TABLE comments ALTER COLUMN created_at TYPE TIMESTAMPTZ
    USING created_at AT TIME ZONE 'UTC';
ALTER TABLE users ALTER COLUMN created_at TYPE TIMESTAMPTZ
    USING created_at AT TIME ZONE 'UTC';
ALTER TABLE attachments ALTER COLUMN created_at TYPE TIMESTAMPTZ
    USING created_at AT TIME ZONE 'UTC';
ALTER TABLE note_shares ALTER COLUMN created_at TYPE TIMESTAMPTZ
    USING created_at AT TIME ZONE 'UTC';

-- Keyset pagination over the chronological note listing.
CREATE INDEX IF NOT EXISTS idx_notes_created_at_id ON notes (created_at DESC, id DESC);
