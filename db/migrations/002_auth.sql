-- 1. Create users table
CREATE TABLE IF NOT EXISTS users (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    email VARCHAR(255) UNIQUE NOT NULL,
    password_hash VARCHAR(255) NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- 2. Insert a fallback/system user to own orphan notes
INSERT INTO users (id, email, password_hash)
VALUES ('00000000-0000-0000-0000-000000000000', 'system@notenest.com', 'SYSTEM_ACCOUNT_DISABLED')
ON CONFLICT (id) DO NOTHING;

-- 3. Add owner_id to notes table as nullable first
ALTER TABLE notes ADD COLUMN IF NOT EXISTS owner_id UUID REFERENCES users(id) ON DELETE CASCADE;

-- 4. Backfill existing notes to belong to the system/fallback user
UPDATE notes SET owner_id = '00000000-0000-0000-0000-000000000000' WHERE owner_id IS NULL;

-- 5. Set owner_id column to NOT NULL
ALTER TABLE notes ALTER COLUMN owner_id SET NOT NULL;
