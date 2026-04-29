#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sqlite3.h>
#include <sys/stat.h>

#include "db.h"
#include "util.h"
#include "registry.h"
#include "model.h"   /* model_read_gguf_meta() */

static sqlite3 *db = NULL;
static char active_model[256] = "";

/* serialize every SQLite call behind a single recursive mutex. we don't trust
   the system libsqlite3 to be in serialized threading mode on every platform
   (macOS system build varies; some Linux distros ship THREADSAFE=0). recursive
   so helpers like db_model_add() can be called from inside db_models_sync()
   without deadlocking — same for db_session_touch() from db_message_add(). */
static pthread_mutex_t db_lock;
static pthread_once_t  db_lock_once = PTHREAD_ONCE_INIT;

static void db_lock_init(void)
{
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&db_lock, &attr);
	pthread_mutexattr_destroy(&attr);
}

#define DB_LOCK()    do { pthread_once(&db_lock_once, db_lock_init); \
                          pthread_mutex_lock(&db_lock); } while (0)
#define DB_UNLOCK()  pthread_mutex_unlock(&db_lock)

/* schema embedded at compile time from src/sql/schema.sql */
#include "schema_sql.h"

/* --- schema migrations ---
   schema.sql defines the head state — fresh DBs jump straight to it.
   migrations[] is append-only: index i is the SQL to take an existing
   DB from version i to i+1. tracked via PRAGMA user_version. */
static const char *migrations[] = {
	/* v0 -> v1: pre-registry models gain a display_name column. */
	"ALTER TABLE models ADD COLUMN display_name TEXT;",
	/* v1 -> v2: (legacy) added status column; kept for upgrade path. */
	"ALTER TABLE messages ADD COLUMN status TEXT DEFAULT 'ok';",
	/* v2 -> v3: delete failed messages instead of keeping them tagged.
	   the column stays (SQLite < 3.35 can't DROP COLUMN) but is unused. */
	"DELETE FROM messages WHERE status = 'failed';",
	/* v3 -> v4: messages.model was added to schema.sql but never migrated.
	   existing DBs created before the column was introduced are missing it,
	   which causes db_message_add INSERTs to silently fail. */
	"ALTER TABLE messages ADD COLUMN model TEXT;",
};
#define MIGRATIONS_HEAD ((int)(sizeof migrations / sizeof migrations[0]))

static int db_user_version(void)
{
	sqlite3_stmt *st;
	int v = 0;
	if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &st, NULL) == SQLITE_OK) {
		if (sqlite3_step(st) == SQLITE_ROW)
			v = sqlite3_column_int(st, 0);
		sqlite3_finalize(st);
	}
	return v;
}

static void db_set_user_version(int v)
{
	char buf[64];
	snprintf(buf, sizeof buf, "PRAGMA user_version = %d;", v);
	sqlite3_exec(db, buf, NULL, NULL, NULL);
}

static int db_run_migrations(int from)
{
	for (int i = from; i < MIGRATIONS_HEAD; i++) {
		char *err = NULL;
		int rc = sqlite3_exec(db, migrations[i], NULL, NULL, &err);
		if (rc != SQLITE_OK) {
			/* legacy DBs from before user_version was tracked may have
			   already received some of these ALTERs via the old ad-hoc
			   path that ignored "duplicate column" errors. treat that
			   specific failure as already-applied so we can advance
			   user_version and move on to the next migration. */
			int already = err && strstr(err, "duplicate column") != NULL;
			if (!already) {
				fprintf(stderr, "kora: migration %d -> %d failed: %s\n",
				        i, i + 1, err ? err : "(unknown)");
				sqlite3_free(err);
				return -1;
			}
			sqlite3_free(err);
		}
		db_set_user_version(i + 1);
	}
	return 0;
}

static int has_suffix(const char *str, const char *suffix)
{
	size_t slen = strlen(str);
	size_t xlen = strlen(suffix);
	if (xlen > slen)
		return 0;
	return strcmp(str + slen - xlen, suffix) == 0;
}

static const char *filename_from_url(const char *url)
{
	const char *slash = strrchr(url, '/');
	return slash ? slash + 1 : url;
}

int db_open(void)
{
	char *path;
	char *err = NULL;
	int rc = 0;

	DB_LOCK();

	path = kora_path("kora.db");
	if (!path) { rc = -1; goto out; }

	if (sqlite3_open(path, &db) != SQLITE_OK) {
		fprintf(stderr, "kora: failed to open database: %s\n",
			sqlite3_errmsg(db));
		free(path);
		rc = -1;
		goto out;
	}
	free(path);

	/* fresh-vs-existing detection has to happen before schema.sql runs,
	   because CREATE TABLE IF NOT EXISTS is silent either way. fresh DBs
	   skip the migration runner entirely (schema.sql is already at HEAD)
	   and just stamp user_version; existing DBs run pending migrations. */
	int fresh = 1;
	{
		sqlite3_stmt *st;
		if (sqlite3_prepare_v2(db,
		    "SELECT count(*) FROM sqlite_master WHERE type='table';",
		    -1, &st, NULL) == SQLITE_OK) {
			if (sqlite3_step(st) == SQLITE_ROW)
				fresh = sqlite3_column_int(st, 0) == 0;
			sqlite3_finalize(st);
		}
	}

	if (sqlite3_exec(db, (const char *)schema_sql_data, NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "kora: schema error: %s\n", err);
		sqlite3_free(err);
		rc = -1;
		goto out;
	}

	if (fresh) {
		db_set_user_version(MIGRATIONS_HEAD);
	} else if (db_run_migrations(db_user_version()) != 0) {
		rc = -1;
		goto out;
	}

	/* WAL mode for better concurrency */
	sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);

out:
	DB_UNLOCK();
	return rc;
}

void db_close(void)
{
	DB_LOCK();
	if (db) {
		sqlite3_close(db);
		db = NULL;
	}
	DB_UNLOCK();
}

sqlite3 *db_handle(void)
{
	return db;
}

/* --- models --- */

void db_models_sync(void)
{
	int i;
	sqlite3_stmt *stmt;
	char *models_dir;
	DIR *d;
	struct dirent *ent;

	DB_LOCK();
	if (!db) goto out;

	/* upsert every registry entry */
	const char *upsert_sql =
		"INSERT INTO models (alias, filename, url, size, quant, source) "
		"VALUES (?, ?, ?, ?, ?, 'registry') "
		"ON CONFLICT(alias) DO UPDATE SET "
		"filename=excluded.filename, url=excluded.url, "
		"size=excluded.size, quant=excluded.quant, source='registry';";

	for (i = 0; registry[i].alias; i++) {
		if (sqlite3_prepare_v2(db, upsert_sql, -1, &stmt, NULL) != SQLITE_OK)
			continue;
		sqlite3_bind_text(stmt, 1, registry[i].alias, -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt, 2, filename_from_url(registry[i].url), -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt, 3, registry[i].url, -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt, 4, registry[i].size, -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt, 5, registry[i].quant, -1, SQLITE_STATIC);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}

	/* scan ~/.kora/models/ and update download status */
	models_dir = kora_path("models");
	if (!models_dir) goto out;

	/* first, mark all as not downloaded */
	sqlite3_exec(db, "UPDATE models SET downloaded = 0;", NULL, NULL, NULL);

	/* heal old broken names: files saved before the filename sanitizer
	   landed can carry URL query strings (e.g. "foo.gguf?download=true").
	   detect "*.gguf?*" and rename to strip the tail so the regular
	   has_suffix(".gguf") scan below finds them. */
	d = opendir(models_dir);
	if (d) {
		while ((ent = readdir(d)) != NULL) {
			const char *q = strstr(ent->d_name, ".gguf?");
			if (!q) continue;
			size_t keep = (size_t)(q - ent->d_name) + 5;  /* up to and including ".gguf" */
			char clean[512];
			if (keep >= sizeof clean) continue;
			memcpy(clean, ent->d_name, keep);
			clean[keep] = '\0';

			char old_path[1024], new_path[1024];
			snprintf(old_path, sizeof old_path, "%s/%s", models_dir, ent->d_name);
			snprintf(new_path, sizeof new_path, "%s/%s", models_dir, clean);

			/* don't clobber a good file that already exists under the
			   clean name — the query-tailed copy is the stale one. */
			struct stat st;
			if (stat(new_path, &st) != 0)
				(void)rename(old_path, new_path);
		}
		closedir(d);
	}

	d = opendir(models_dir);
	if (d) {
		while ((ent = readdir(d)) != NULL) {
			if (!has_suffix(ent->d_name, ".gguf"))
				continue;

			/* try to match against known models by filename */
			const char *mark_sql =
				"UPDATE models SET downloaded = 1 WHERE filename = ?;";
			if (sqlite3_prepare_v2(db, mark_sql, -1, &stmt, NULL) == SQLITE_OK) {
				sqlite3_bind_text(stmt, 1, ent->d_name, -1, SQLITE_STATIC);
				sqlite3_step(stmt);
				sqlite3_finalize(stmt);
			}

			/* if no row was updated, this is a manually downloaded model.
			   db_model_add re-locks — recursive mutex makes that safe. */
			if (sqlite3_changes(db) == 0) {
				db_model_add(ent->d_name);
			}
		}
		closedir(d);
	}

	free(models_dir);

	/* load active model into static buffer */
	active_model[0] = '\0';
	if (sqlite3_prepare_v2(db,
		"SELECT alias FROM models WHERE active = 1 LIMIT 1;",
		-1, &stmt, NULL) == SQLITE_OK) {
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *a = (const char *)sqlite3_column_text(stmt, 0);
			if (a) snprintf(active_model, sizeof(active_model), "%s", a);
		}
		sqlite3_finalize(stmt);
	}

out:
	DB_UNLOCK();
}

int db_model_is_downloaded(const char *name)
{
	sqlite3_stmt *stmt;
	int result = 0;

	DB_LOCK();
	if (!db) goto out;

	if (sqlite3_prepare_v2(db,
		"SELECT downloaded FROM models WHERE alias = ? OR filename = ?;",
		-1, &stmt, NULL) != SQLITE_OK)
		goto out;

	sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
	if (sqlite3_step(stmt) == SQLITE_ROW)
		result = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);

out:
	DB_UNLOCK();
	return result;
}

void db_model_set_downloaded(const char *name, int downloaded)
{
	sqlite3_stmt *stmt;

	DB_LOCK();
	if (!db) goto out;

	if (sqlite3_prepare_v2(db,
		"UPDATE models SET downloaded = ? WHERE alias = ? OR filename = ?;",
		-1, &stmt, NULL) != SQLITE_OK)
		goto out;

	sqlite3_bind_int(stmt, 1, downloaded);
	sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 3, name, -1, SQLITE_STATIC);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

out:
	DB_UNLOCK();
}

void db_models_each(db_model_cb cb, void *user_data)
{
	sqlite3_stmt *stmt;

	DB_LOCK();
	if (!db) goto out;

	if (sqlite3_prepare_v2(db,
		"SELECT alias, filename, size, quant, downloaded, active, display_name "
		"FROM models ORDER BY source ASC, alias ASC;",
		-1, &stmt, NULL) != SQLITE_OK)
		goto out;

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *alias    = (const char *)sqlite3_column_text(stmt, 0);
		const char *filename = (const char *)sqlite3_column_text(stmt, 1);
		const char *size     = (const char *)sqlite3_column_text(stmt, 2);
		const char *quant    = (const char *)sqlite3_column_text(stmt, 3);
		int downloaded       = sqlite3_column_int(stmt, 4);
		int active           = sqlite3_column_int(stmt, 5);
		const char *display  = (const char *)sqlite3_column_text(stmt, 6);

		cb(alias, filename, size ? size : "-", quant ? quant : "-",
		   downloaded, active, display, user_data);
	}
	sqlite3_finalize(stmt);

out:
	DB_UNLOCK();
}

void db_model_add(const char *filename)
{
	sqlite3_stmt *stmt;
	char alias[256];
	char *dot;

	DB_LOCK();
	if (!db) goto out;

	/* derive alias from filename: strip .gguf */
	snprintf(alias, sizeof(alias), "%s", filename);
	dot = strstr(alias, ".gguf");
	if (dot) *dot = '\0';

	/* peek into the GGUF header for a human-readable `general.name` —
	   used only as a display hint in the MODELS pane; the alias above
	   remains the internal identifier for API / routing / rm. failure
	   is non-fatal (legacy files / odd quantisers just leave the
	   display_name column NULL). */
	char display[160] = "";
	{
		char sub[512];
		snprintf(sub, sizeof sub, "models/%s", filename);
		char *path = kora_path(sub);
		if (path) {
			model_read_gguf_meta(path, display, sizeof display,
			                     NULL, 0);
			free(path);
		}
	}

	const char *sql =
		"INSERT INTO models (alias, filename, source, downloaded, display_name) "
		"VALUES (?, ?, 'manual', 1, ?) "
		"ON CONFLICT(alias) DO UPDATE SET "
		"  downloaded   = 1, "
		"  display_name = COALESCE(NULLIF(excluded.display_name, ''), display_name);";

	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
		goto out;

	sqlite3_bind_text(stmt, 1, alias, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, filename, -1, SQLITE_TRANSIENT);
	if (display[0])
		sqlite3_bind_text(stmt, 3, display, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 3);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

out:
	DB_UNLOCK();
}

void db_model_set_active(const char *name)
{
	sqlite3_stmt *stmt;

	DB_LOCK();
	if (!db) goto out;

	/* clear all active flags */
	sqlite3_exec(db, "UPDATE models SET active = 0;", NULL, NULL, NULL);

	if (sqlite3_prepare_v2(db,
		"UPDATE models SET active = 1 WHERE alias = ? OR filename = ?;",
		-1, &stmt, NULL) != SQLITE_OK)
		goto out;

	sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	snprintf(active_model, sizeof(active_model), "%s", name);

out:
	DB_UNLOCK();
}

const char *db_model_get_active(void)
{
	const char *result;
	DB_LOCK();
	result = active_model[0] ? active_model : NULL;
	DB_UNLOCK();
	return result;
}

/* --- sessions --- */

int db_session_create(const char *mode, const char *model, const char *cwd)
{
	sqlite3_stmt *stmt;
	int rc = -1;

	DB_LOCK();
	if (!db) goto out;

	const char *sql =
		"INSERT INTO sessions (mode, model, cwd) VALUES (?, ?, ?);";

	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
		goto out;

	sqlite3_bind_text(stmt, 1, mode, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 2, model, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 3, cwd, -1, SQLITE_STATIC);

	if (sqlite3_step(stmt) == SQLITE_DONE)
		rc = (int)sqlite3_last_insert_rowid(db);
	sqlite3_finalize(stmt);

out:
	DB_UNLOCK();
	return rc;
}

void db_session_set_name(int session_id, const char *name)
{
	sqlite3_stmt *stmt;

	DB_LOCK();
	if (!db) goto out;

	if (sqlite3_prepare_v2(db,
		"UPDATE sessions SET name = ?, updated_at = datetime('now') WHERE id = ?;",
		-1, &stmt, NULL) != SQLITE_OK)
		goto out;

	sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt, 2, session_id);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

out:
	DB_UNLOCK();
}

void db_session_touch(int session_id)
{
	sqlite3_stmt *stmt;

	DB_LOCK();
	if (!db) goto out;

	if (sqlite3_prepare_v2(db,
		"UPDATE sessions SET updated_at = datetime('now') WHERE id = ?;",
		-1, &stmt, NULL) != SQLITE_OK)
		goto out;

	sqlite3_bind_int(stmt, 1, session_id);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

out:
	DB_UNLOCK();
}

void db_session_delete(int session_id)
{
	sqlite3_stmt *stmt;

	DB_LOCK();
	if (!db) goto out;

	/* delete messages first */
	if (sqlite3_prepare_v2(db,
		"DELETE FROM messages WHERE session_id = ?;",
		-1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_int(stmt, 1, session_id);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}

	if (sqlite3_prepare_v2(db,
		"DELETE FROM sessions WHERE id = ?;",
		-1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_int(stmt, 1, session_id);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}

out:
	DB_UNLOCK();
}

int db_sessions_list(struct db_session *out, int max)
{
	sqlite3_stmt *stmt;
	int count = 0;

	DB_LOCK();
	if (!db) goto done;

	const char *sql =
		"SELECT s.id, s.name, s.mode, s.model, s.created_at, s.updated_at, "
		"  (SELECT COUNT(*) FROM messages WHERE session_id = s.id) AS msg_count, "
		"  (SELECT content FROM messages WHERE session_id = s.id "
		"   AND role = 'user' ORDER BY seq DESC LIMIT 1) AS last_msg "
		"FROM sessions s ORDER BY s.updated_at DESC LIMIT ?;";

	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
		goto done;

	sqlite3_bind_int(stmt, 1, max);

	while (sqlite3_step(stmt) == SQLITE_ROW && count < max) {
		struct db_session *s = &out[count];
		const char *val;

		s->id = sqlite3_column_int(stmt, 0);

		val = (const char *)sqlite3_column_text(stmt, 1);
		snprintf(s->name, sizeof(s->name), "%s", val ? val : "New session");

		val = (const char *)sqlite3_column_text(stmt, 2);
		snprintf(s->mode, sizeof(s->mode), "%s", val ? val : "chat");

		val = (const char *)sqlite3_column_text(stmt, 3);
		snprintf(s->model, sizeof(s->model), "%s", val ? val : "");

		val = (const char *)sqlite3_column_text(stmt, 4);
		snprintf(s->created_at, sizeof(s->created_at), "%s", val ? val : "");

		val = (const char *)sqlite3_column_text(stmt, 5);
		snprintf(s->updated_at, sizeof(s->updated_at), "%s", val ? val : "");

		s->message_count = sqlite3_column_int(stmt, 6);

		val = (const char *)sqlite3_column_text(stmt, 7);
		snprintf(s->last_message, sizeof(s->last_message), "%s", val ? val : "");

		count++;
	}
	sqlite3_finalize(stmt);

done:
	DB_UNLOCK();
	return count;
}

int db_session_get(int session_id, struct db_session *out)
{
	sqlite3_stmt *stmt;
	int rc = -1;

	DB_LOCK();
	if (!db) goto done;

	const char *sql =
		"SELECT s.id, s.name, s.mode, s.model, s.created_at, s.updated_at, "
		"  (SELECT COUNT(*) FROM messages WHERE session_id = s.id) AS msg_count, "
		"  (SELECT content FROM messages WHERE session_id = s.id "
		"   AND role = 'user' ORDER BY seq DESC LIMIT 1) AS last_msg "
		"FROM sessions s WHERE s.id = ?;";

	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
		goto done;

	sqlite3_bind_int(stmt, 1, session_id);

	if (sqlite3_step(stmt) != SQLITE_ROW) {
		sqlite3_finalize(stmt);
		goto done;
	}

	const char *val;
	out->id = sqlite3_column_int(stmt, 0);

	val = (const char *)sqlite3_column_text(stmt, 1);
	snprintf(out->name, sizeof(out->name), "%s", val ? val : "New session");

	val = (const char *)sqlite3_column_text(stmt, 2);
	snprintf(out->mode, sizeof(out->mode), "%s", val ? val : "chat");

	val = (const char *)sqlite3_column_text(stmt, 3);
	snprintf(out->model, sizeof(out->model), "%s", val ? val : "");

	val = (const char *)sqlite3_column_text(stmt, 4);
	snprintf(out->created_at, sizeof(out->created_at), "%s", val ? val : "");

	val = (const char *)sqlite3_column_text(stmt, 5);
	snprintf(out->updated_at, sizeof(out->updated_at), "%s", val ? val : "");

	out->message_count = sqlite3_column_int(stmt, 6);

	val = (const char *)sqlite3_column_text(stmt, 7);
	snprintf(out->last_message, sizeof(out->last_message), "%s", val ? val : "");

	sqlite3_finalize(stmt);
	rc = 0;

done:
	DB_UNLOCK();
	return rc;
}

/* --- messages --- */

void db_message_add(int session_id, int seq, const char *role,
                    const char *content, const char *model, int llm_use)
{
	sqlite3_stmt *stmt;

	DB_LOCK();
	if (!db) goto out;

	const char *sql =
		"INSERT INTO messages (session_id, seq, role, content, model, llm_use) "
		"VALUES (?, ?, ?, ?, ?, ?);";

	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
		goto out;

	sqlite3_bind_int(stmt, 1, session_id);
	sqlite3_bind_int(stmt, 2, seq);
	sqlite3_bind_text(stmt, 3, role, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 4, content, -1, SQLITE_STATIC);
	if (model)
		sqlite3_bind_text(stmt, 5, model, -1, SQLITE_STATIC);
	else
		sqlite3_bind_null(stmt, 5);
	sqlite3_bind_int(stmt, 6, llm_use);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	db_session_touch(session_id);  /* recursive lock — safe */

out:
	DB_UNLOCK();
}

void db_message_delete(int session_id, int seq)
{
	sqlite3_stmt *stmt;

	DB_LOCK();
	if (!db) goto out;

	const char *sql =
		"DELETE FROM messages "
		"WHERE session_id = ? AND seq = ?;";

	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
		goto out;

	sqlite3_bind_int(stmt, 1, session_id);
	sqlite3_bind_int(stmt, 2, seq);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

out:
	DB_UNLOCK();
}

int db_messages_load(int session_id, struct db_message **out)
{
	sqlite3_stmt *stmt;
	int count = 0;
	int cap = 64;
	struct db_message *msgs;

	DB_LOCK();

	if (!db) { *out = NULL; goto done; }

	msgs = malloc(cap * sizeof(*msgs));
	if (!msgs) { *out = NULL; goto done; }

	const char *sql =
		"SELECT seq, role, content, model, llm_use FROM messages "
		"WHERE session_id = ? ORDER BY seq ASC;";

	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
		free(msgs);
		*out = NULL;
		goto done;
	}

	sqlite3_bind_int(stmt, 1, session_id);

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		if (count >= cap) {
			cap *= 2;
			struct db_message *tmp = realloc(msgs, cap * sizeof(*msgs));
			if (!tmp) { *out = msgs; sqlite3_finalize(stmt); goto done; }
			msgs = tmp;
		}
		msgs[count].seq = sqlite3_column_int(stmt, 0);
		const char *r = (const char *)sqlite3_column_text(stmt, 1);
		const char *c = (const char *)sqlite3_column_text(stmt, 2);
		const char *m = (const char *)sqlite3_column_text(stmt, 3);
		msgs[count].role = r ? strdup(r) : strdup("");
		msgs[count].content = c ? strdup(c) : strdup("");
		msgs[count].model = m ? strdup(m) : NULL;
		msgs[count].llm_use = sqlite3_column_int(stmt, 4);
		count++;
	}
	sqlite3_finalize(stmt);
	*out = msgs;

done:
	DB_UNLOCK();
	return count;
}

void db_messages_free(struct db_message *msgs, int count)
{
	int i;
	if (!msgs) return;
	for (i = 0; i < count; i++) {
		free(msgs[i].role);
		free(msgs[i].content);
		free(msgs[i].model);
	}
	free(msgs);
}

/* --- settings --- */

char *db_setting_get(const char *key)
{
	sqlite3_stmt *stmt;
	char *result = NULL;

	DB_LOCK();
	if (!db) goto out;

	if (sqlite3_prepare_v2(db,
		"SELECT value FROM settings WHERE key = ?;",
		-1, &stmt, NULL) != SQLITE_OK)
		goto out;

	sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *val = (const char *)sqlite3_column_text(stmt, 0);
		if (val) result = strdup(val);
	}
	sqlite3_finalize(stmt);

out:
	DB_UNLOCK();
	return result;
}

void db_setting_set(const char *key, const char *value)
{
	sqlite3_stmt *stmt;

	DB_LOCK();
	if (!db) goto out;

	if (sqlite3_prepare_v2(db,
		"INSERT INTO settings (key, value) VALUES (?, ?) "
		"ON CONFLICT(key) DO UPDATE SET value = excluded.value;",
		-1, &stmt, NULL) != SQLITE_OK)
		goto out;

	sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 2, value, -1, SQLITE_STATIC);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

out:
	DB_UNLOCK();
}
