-- Create attachments table to store file/image metadata associated with notes
CREATE TABLE IF NOT EXISTS attachments (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    note_id UUID NOT NULL REFERENCES notes(id) ON DELETE CASCADE,
    bucket VARCHAR(255) NOT NULL,
    key VARCHAR(255) NOT NULL,
    size BIGINT NOT NULL,
    filename VARCHAR(255) NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
