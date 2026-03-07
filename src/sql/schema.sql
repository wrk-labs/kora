-- kora database schema
-- ~/.kora/kora.db

CREATE TABLE IF NOT EXISTS models (
    alias       TEXT PRIMARY KEY,
    filename    TEXT NOT NULL,
    url         TEXT,
    size        TEXT,
    quant       TEXT,
    downloaded  INTEGER DEFAULT 0,
    active      INTEGER DEFAULT 0,
    source      TEXT DEFAULT 'registry'  -- 'registry' or 'manual'
);

CREATE TABLE IF NOT EXISTS settings (
    key   TEXT PRIMARY KEY,
    value TEXT
);
