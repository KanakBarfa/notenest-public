-- Migration 005: Add permission column to note_shares table
ALTER TABLE note_shares ADD COLUMN IF NOT EXISTS permission VARCHAR(20) NOT NULL DEFAULT 'editor';
