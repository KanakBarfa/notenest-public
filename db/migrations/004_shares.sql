-- Create note_shares table to store user access rights for shared notes
CREATE TABLE IF NOT EXISTS note_shares (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    note_id UUID NOT NULL REFERENCES notes(id) ON DELETE CASCADE,
    shared_with_user_id UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    CONSTRAINT unique_note_share UNIQUE (note_id, shared_with_user_id)
);
