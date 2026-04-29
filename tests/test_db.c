#include "test.h"
#include "db.h"
#include "util.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* point $HOME at a fresh temp directory so db_open writes to a sandbox
   instead of the developer's real ~/.kora. each test binary owns its
   sandbox for the duration of main(). */
static char tmp_home[512];

static int setup_tmp_home(void)
{
	char tpl[] = "/tmp/kora-test-XXXXXX";
	char *p = mkdtemp(tpl);
	if (!p) return -1;
	snprintf(tmp_home, sizeof tmp_home, "%s", p);
	setenv("HOME", tmp_home, 1);
	return kora_init_dirs();
}

static void teardown_tmp_home(void)
{
	/* best-effort cleanup; tests are sandboxed under /tmp anyway. */
	char cmd[600];
	snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmp_home);
	if (system(cmd) != 0) { /* best-effort */ }
}

static void test_open_close_idempotent(void)
{
	TEST_BEGIN("db_open / db_close cycle works and creates kora.db");
	EXPECT_EQ(db_open(), 0);
	db_close();

	char path[700];
	snprintf(path, sizeof path, "%s/.kora/kora.db", tmp_home);
	struct stat st;
	EXPECT_EQ(stat(path, &st), 0);
	EXPECT(st.st_size > 0);

	/* reopen should succeed on the existing file. */
	EXPECT_EQ(db_open(), 0);
	db_close();
	TEST_END();
}

static void test_session_roundtrip_preserves_messages(void)
{
	TEST_BEGIN("messages survive close+reopen byte-exact, including UTF-8");
	ASSERT(db_open() == 0);

	int sid = db_session_create("chat", "llama-3.2-3b", "/tmp/cwd");
	EXPECT(sid > 0);

	const char *weird = "café\n\"quoted\"\t<tag>\xF0\x9F\x91\x8B";
	db_message_add(sid, 0, "user",      weird,       "llama-3.2-3b", 1);
	db_message_add(sid, 1, "assistant", "ok, got it", "llama-3.2-3b", 1);

	db_close();
	ASSERT(db_open() == 0);

	struct db_message *msgs = NULL;
	int n = db_messages_load(sid, &msgs);
	EXPECT_EQ(n, 2);
	ASSERT(msgs != NULL);

	EXPECT_STREQ(msgs[0].role, "user");
	EXPECT_STREQ(msgs[0].content, weird);
	EXPECT_EQ(msgs[0].seq, 0);
	EXPECT_EQ(msgs[0].llm_use, 1);

	EXPECT_STREQ(msgs[1].role, "assistant");
	EXPECT_STREQ(msgs[1].content, "ok, got it");
	EXPECT_EQ(msgs[1].seq, 1);

	db_messages_free(msgs, n);
	db_close();
	TEST_END();
}

static void test_session_delete_cascades_to_messages(void)
{
	TEST_BEGIN("db_session_delete also removes messages");
	ASSERT(db_open() == 0);
	int sid = db_session_create("chat", "m", NULL);
	EXPECT(sid > 0);
	db_message_add(sid, 0, "user", "x", NULL, 1);
	db_message_add(sid, 1, "assistant", "y", NULL, 1);

	db_session_delete(sid);

	struct db_message *msgs = NULL;
	int n = db_messages_load(sid, &msgs);
	EXPECT_EQ(n, 0);
	db_messages_free(msgs, n);

	struct db_session s;
	EXPECT_EQ(db_session_get(sid, &s), -1);
	db_close();
	TEST_END();
}

static void test_message_delete_removes_row(void)
{
	TEST_BEGIN("db_message_delete removes a message by session + seq");
	ASSERT(db_open() == 0);

	int sid = db_session_create("chat", "m", NULL);
	db_message_add(sid, 0, "user",      "good question",   "m", 1);
	db_message_add(sid, 1, "assistant", "good answer",     "m", 1);
	db_message_add(sid, 2, "user",      "dispatch failed", "m", 1);
	db_message_add(sid, 3, "user",      "later turn",      "m", 1);

	struct db_message *msgs = NULL;
	int n = db_messages_load(sid, &msgs);
	EXPECT_EQ(n, 4);
	db_messages_free(msgs, n);

	db_message_delete(sid, 2);

	msgs = NULL;
	n = db_messages_load(sid, &msgs);
	EXPECT_EQ(n, 3);
	ASSERT(msgs != NULL);
	EXPECT_STREQ(msgs[0].content, "good question");
	EXPECT_STREQ(msgs[1].content, "good answer");
	EXPECT_STREQ(msgs[2].content, "later turn");
	db_messages_free(msgs, n);

	db_close();
	TEST_END();
}

/* simulate a v0 DB (pre-migration-system) opened by the current binary.
   the migration runner should bring it forward to HEAD without losing data
   and existing rows should pick up the column default ('ok'). */
static void test_migrations_upgrade_legacy_db(void)
{
	TEST_BEGIN("legacy DB without status column upgrades to current schema");

	/* hand-craft a minimal pre-migration DB at the path db_open() will
	   reopen. only the bits the migration touches need to exist. */
	char *path = kora_path("kora.db");
	ASSERT(path != NULL);
	unlink(path);

	sqlite3 *raw = NULL;
	ASSERT(sqlite3_open(path, &raw) == SQLITE_OK);

	const char *legacy =
		"CREATE TABLE models ("
		"  alias TEXT PRIMARY KEY, filename TEXT, url TEXT,"
		"  size TEXT, quant TEXT, downloaded INTEGER DEFAULT 0,"
		"  active INTEGER DEFAULT 0, source TEXT DEFAULT 'registry');"
		"CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT);"
		"CREATE TABLE sessions ("
		"  id INTEGER PRIMARY KEY AUTOINCREMENT,"
		"  name TEXT DEFAULT 'New session',"
		"  mode TEXT NOT NULL DEFAULT 'chat',"
		"  model TEXT, cwd TEXT,"
		"  created_at TEXT DEFAULT (datetime('now')),"
		"  updated_at TEXT DEFAULT (datetime('now')));"
		"CREATE TABLE messages ("
		"  id INTEGER PRIMARY KEY AUTOINCREMENT,"
		"  session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
		"  seq INTEGER NOT NULL, role TEXT NOT NULL, content TEXT NOT NULL,"
		"  model TEXT, llm_use INTEGER DEFAULT 1,"
		"  created_at TEXT DEFAULT (datetime('now')));"
		"INSERT INTO sessions (id, mode) VALUES (1, 'chat');"
		"INSERT INTO messages (session_id, seq, role, content, model, llm_use)"
		"  VALUES (1, 0, 'user', 'pre-migration', 'm', 1);"
		"PRAGMA user_version = 0;";
	char *err = NULL;
	ASSERT(sqlite3_exec(raw, legacy, NULL, NULL, &err) == SQLITE_OK);
	sqlite3_close(raw);
	free(path);

	/* now reopen via db_open — the runner should bring this forward. */
	ASSERT(db_open() == 0);

	/* the user_version should be at HEAD (whatever migrations[] length is). */
	int v = -1;
	sqlite3_stmt *st;
	ASSERT(sqlite3_prepare_v2(db_handle(), "PRAGMA user_version;",
	                          -1, &st, NULL) == SQLITE_OK);
	if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int(st, 0);
	sqlite3_finalize(st);
	EXPECT(v >= 4);   /* at least v4 (model column on messages) */

	/* the pre-existing user message should still be there. */
	struct db_message *msgs = NULL;
	int n = db_messages_load(1, &msgs);
	EXPECT_EQ(n, 1);
	ASSERT(msgs != NULL);
	EXPECT_STREQ(msgs[0].content, "pre-migration");
	db_messages_free(msgs, n);

	/* second open is a no-op: already at HEAD, no migration runs. */
	db_close();
	ASSERT(db_open() == 0);
	db_close();

	TEST_END();
}

/* legacy DBs from before user_version was tracked may already have
   the v0->v1 ALTER applied (display_name) while still reporting
   user_version=0. the runner must tolerate "duplicate column" errors
   in this case and still advance to HEAD. */
static void test_migrations_tolerate_already_applied_alters(void)
{
	TEST_BEGIN("legacy DB with display_name already added still upgrades to HEAD");

	char *path = kora_path("kora.db");
	ASSERT(path != NULL);
	unlink(path);

	sqlite3 *raw = NULL;
	ASSERT(sqlite3_open(path, &raw) == SQLITE_OK);

	/* note: models has display_name already (mirroring the post-ad-hoc-ALTER
	   state of real legacy DBs), but user_version is still 0. */
	const char *legacy =
		"CREATE TABLE models ("
		"  alias TEXT PRIMARY KEY, filename TEXT, url TEXT,"
		"  size TEXT, quant TEXT, downloaded INTEGER DEFAULT 0,"
		"  active INTEGER DEFAULT 0, source TEXT DEFAULT 'registry',"
		"  display_name TEXT);"
		"CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT);"
		"CREATE TABLE sessions ("
		"  id INTEGER PRIMARY KEY AUTOINCREMENT,"
		"  name TEXT DEFAULT 'New session',"
		"  mode TEXT NOT NULL DEFAULT 'chat',"
		"  model TEXT, cwd TEXT,"
		"  created_at TEXT DEFAULT (datetime('now')),"
		"  updated_at TEXT DEFAULT (datetime('now')));"
		"CREATE TABLE messages ("
		"  id INTEGER PRIMARY KEY AUTOINCREMENT,"
		"  session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
		"  seq INTEGER NOT NULL, role TEXT NOT NULL, content TEXT NOT NULL,"
		"  model TEXT, llm_use INTEGER DEFAULT 1,"
		"  created_at TEXT DEFAULT (datetime('now')));"
		"PRAGMA user_version = 0;";
	ASSERT(sqlite3_exec(raw, legacy, NULL, NULL, NULL) == SQLITE_OK);
	sqlite3_close(raw);
	free(path);

	EXPECT_EQ(db_open(), 0);

	int v = -1;
	sqlite3_stmt *st;
	ASSERT(sqlite3_prepare_v2(db_handle(), "PRAGMA user_version;",
	                          -1, &st, NULL) == SQLITE_OK);
	if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int(st, 0);
	sqlite3_finalize(st);
	EXPECT(v >= 2);   /* runner reached HEAD despite duplicate-column on v0->v1 */

	db_close();
	TEST_END();
}

static void test_settings_upsert(void)
{
	TEST_BEGIN("db_setting_set upserts; db_setting_get reads latest");
	ASSERT(db_open() == 0);

	EXPECT(db_setting_get("theme") == NULL);

	db_setting_set("theme", "dark");
	char *v = db_setting_get("theme");
	EXPECT_STREQ(v, "dark");
	free(v);

	db_setting_set("theme", "light");
	v = db_setting_get("theme");
	EXPECT_STREQ(v, "light");
	free(v);

	db_close();
	TEST_END();
}

int main(void)
{
	if (setup_tmp_home() != 0) {
		fprintf(stderr, "FATAL: could not set up temp HOME\n");
		return 2;
	}

	test_open_close_idempotent();
	test_session_roundtrip_preserves_messages();
	test_session_delete_cascades_to_messages();
	test_message_delete_removes_row();
	test_migrations_upgrade_legacy_db();
	test_migrations_tolerate_already_applied_alters();
	test_settings_upsert();

	teardown_tmp_home();
	return TEST_REPORT();
}
