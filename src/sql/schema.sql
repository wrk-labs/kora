-- kora database schema
-- ~/.kora/kora.db

CREATE TABLE IF NOT EXISTS models (
    alias        TEXT PRIMARY KEY,
    filename     TEXT NOT NULL,
    url          TEXT,
    size         TEXT,
    quant        TEXT,
    downloaded   INTEGER DEFAULT 0,
    active       INTEGER DEFAULT 0,
    source       TEXT DEFAULT 'registry',  -- 'registry' or 'manual'
    display_name TEXT                      -- from GGUF `general.name` for manual pulls
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
    model       TEXT,                -- alias of the model in effect for this turn
    llm_use     INTEGER DEFAULT 1,   -- include in prompt when building context
    status      TEXT DEFAULT 'ok',   -- 'ok' or 'failed' (e.g. daemon down)
    created_at  TEXT DEFAULT (datetime('now'))
);
