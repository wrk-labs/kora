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

CREATE TABLE IF NOT EXISTS sessions (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    name        TEXT DEFAULT 'New session',
    mode        TEXT NOT NULL DEFAULT 'chat',   -- 'chat' or 'code'
    model       TEXT,
    cwd         TEXT,
    created_at  TEXT DEFAULT (datetime('now')),
    updated_at  TEXT DEFAULT (datetime('now'))
);

CREATE TABLE IF NOT EXISTS messages (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id  INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
    seq         INTEGER NOT NULL,
    role        TEXT NOT NULL,       -- 'system', 'user', 'assistant'
    content     TEXT NOT NULL,
    llm_use     INTEGER DEFAULT 1,   -- include in prompt when building context
    created_at  TEXT DEFAULT (datetime('now'))
);
