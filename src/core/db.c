#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sqlite3.h>

#include "db.h"
#include "util.h"
#include "registry.h"

static sqlite3 *db = NULL;
static char active_model[256] = "";

/* schema embedded at compile time from src/sql/schema.sql */
#include "schema_sql.h"

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

	path = kora_path("kora.db");
	if (!path)
		return -1;

	if (sqlite3_open(path, &db) != SQLITE_OK) {
		fprintf(stderr, "kora: failed to open database: %s\n",
			sqlite3_errmsg(db));
		free(path);
		return -1;
	}
	free(path);

	/* create tables if they don't exist */
	if (sqlite3_exec(db, (const char *)schema_sql_data, NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "kora: schema error: %s\n", err);
		sqlite3_free(err);
		return -1;
	}

	/* WAL mode for better concurrency */
	sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);

	return 0;
}

void db_close(void)
{
	if (db) {
		sqlite3_close(db);
		db = NULL;
	}
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

	if (!db) return;

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
	if (!models_dir) return;

	/* first, mark all as not downloaded */
	sqlite3_exec(db, "UPDATE models SET downloaded = 0;", NULL, NULL, NULL);

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

			/* if no row was updated, this is a manually downloaded model */
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
}

int db_model_is_downloaded(const char *name)
{
	sqlite3_stmt *stmt;
	int result = 0;

	if (!db) return 0;

	if (sqlite3_prepare_v2(db,
		"SELECT downloaded FROM models WHERE alias = ? OR filename = ?;",
		-1, &stmt, NULL) != SQLITE_OK)
		return 0;

	sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
	if (sqlite3_step(stmt) == SQLITE_ROW)
		result = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return result;
}

void db_model_set_downloaded(const char *name, int downloaded)
{
	sqlite3_stmt *stmt;

	if (!db) return;

	if (sqlite3_prepare_v2(db,
		"UPDATE models SET downloaded = ? WHERE alias = ? OR filename = ?;",
		-1, &stmt, NULL) != SQLITE_OK)
		return;

	sqlite3_bind_int(stmt, 1, downloaded);
	sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 3, name, -1, SQLITE_STATIC);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
}

void db_models_each(db_model_cb cb, void *user_data)
{
	sqlite3_stmt *stmt;

	if (!db) return;

	if (sqlite3_prepare_v2(db,
		"SELECT alias, filename, size, quant, downloaded, active "
		"FROM models ORDER BY source ASC, alias ASC;",
		-1, &stmt, NULL) != SQLITE_OK)
		return;

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *alias    = (const char *)sqlite3_column_text(stmt, 0);
		const char *filename = (const char *)sqlite3_column_text(stmt, 1);
		const char *size     = (const char *)sqlite3_column_text(stmt, 2);
		const char *quant    = (const char *)sqlite3_column_text(stmt, 3);
		int downloaded       = sqlite3_column_int(stmt, 4);
		int active           = sqlite3_column_int(stmt, 5);

		cb(alias, filename, size ? size : "-", quant ? quant : "-",
		   downloaded, active, user_data);
	}
	sqlite3_finalize(stmt);
}

void db_model_add(const char *filename)
{
	sqlite3_stmt *stmt;
	char alias[256];
	char *dot;

	if (!db) return;

	/* derive alias from filename: strip .gguf */
	snprintf(alias, sizeof(alias), "%s", filename);
	dot = strstr(alias, ".gguf");
	if (dot) *dot = '\0';

	const char *sql =
		"INSERT INTO models (alias, filename, source, downloaded) "
		"VALUES (?, ?, 'manual', 1) "
		"ON CONFLICT(alias) DO UPDATE SET downloaded = 1;";

	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
		return;

	sqlite3_bind_text(stmt, 1, alias, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, filename, -1, SQLITE_TRANSIENT);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
}

void db_model_set_active(const char *name)
{
	sqlite3_stmt *stmt;

	if (!db) return;

	/* clear all active flags */
	sqlite3_exec(db, "UPDATE models SET active = 0;", NULL, NULL, NULL);

	if (sqlite3_prepare_v2(db,
		"UPDATE models SET active = 1 WHERE alias = ? OR filename = ?;",
		-1, &stmt, NULL) != SQLITE_OK)
		return;

	sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	snprintf(active_model, sizeof(active_model), "%s", name);
}

const char *db_model_get_active(void)
{
	return active_model[0] ? active_model : NULL;
}

/* --- sessions --- */

int db_session_create(const char *mode, const char *model, const char *cwd)
{
	sqlite3_stmt *stmt;

	if (!db) return -1;

	const char *sql =
		"INSERT INTO sessions (mode, model, cwd) VALUES (?, ?, ?);";

	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
		return -1;

	sqlite3_bind_text(stmt, 1, mode, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 2, model, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 3, cwd, -1, SQLITE_STATIC);

	if (sqlite3_step(stmt) != SQLITE_DONE) {
		sqlite3_finalize(stmt);
		return -1;
	}
	sqlite3_finalize(stmt);
	return (int)sqlite3_last_insert_rowid(db);
}

void db_session_set_name(int session_id, const char *name)
{
	sqlite3_stmt *stmt;

	if (!db) return;

	if (sqlite3_prepare_v2(db,
		"UPDATE sessions SET name = ?, updated_at = datetime('now') WHERE id = ?;",
		-1, &stmt, NULL) != SQLITE_OK)
		return;

	sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt, 2, session_id);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
}

void db_session_touch(int session_id)
{
	sqlite3_stmt *stmt;

	if (!db) return;

	if (sqlite3_prepare_v2(db,
		"UPDATE sessions SET updated_at = datetime('now') WHERE id = ?;",
		-1, &stmt, NULL) != SQLITE_OK)
		return;

	sqlite3_bind_int(stmt, 1, session_id);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
}

void db_session_delete(int session_id)
{
	sqlite3_stmt *stmt;

	if (!db) return;

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
}

int db_sessions_list(struct db_session *out, int max)
{
	sqlite3_stmt *stmt;
	int count = 0;

	if (!db) return 0;

	const char *sql =
		"SELECT s.id, s.name, s.mode, s.model, s.created_at, s.updated_at, "
		"  (SELECT COUNT(*) FROM messages WHERE session_id = s.id) AS msg_count, "
		"  (SELECT content FROM messages WHERE session_id = s.id "
		"   AND role = 'user' ORDER BY seq DESC LIMIT 1) AS last_msg "
		"FROM sessions s ORDER BY s.updated_at DESC LIMIT ?;";

	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
		return 0;

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
	return count;
}

int db_session_get(int session_id, struct db_session *out)
{
	sqlite3_stmt *stmt;

	if (!db) return -1;

	const char *sql =
		"SELECT s.id, s.name, s.mode, s.model, s.created_at, s.updated_at, "
		"  (SELECT COUNT(*) FROM messages WHERE session_id = s.id) AS msg_count, "
		"  (SELECT content FROM messages WHERE session_id = s.id "
		"   AND role = 'user' ORDER BY seq DESC LIMIT 1) AS last_msg "
		"FROM sessions s WHERE s.id = ?;";

	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
		return -1;

	sqlite3_bind_int(stmt, 1, session_id);

	if (sqlite3_step(stmt) != SQLITE_ROW) {
		sqlite3_finalize(stmt);
		return -1;
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
	return 0;
}

/* --- messages --- */

void db_message_add(int session_id, int seq, const char *role,
                    const char *content, int llm_use)
{
	sqlite3_stmt *stmt;

	if (!db) return;

	const char *sql =
		"INSERT INTO messages (session_id, seq, role, content, llm_use) "
		"VALUES (?, ?, ?, ?, ?);";

	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
		return;

	sqlite3_bind_int(stmt, 1, session_id);
	sqlite3_bind_int(stmt, 2, seq);
	sqlite3_bind_text(stmt, 3, role, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 4, content, -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt, 5, llm_use);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	db_session_touch(session_id);
}

int db_messages_load(int session_id, struct db_message **out)
{
	sqlite3_stmt *stmt;
	int count = 0;
	int cap = 64;
	struct db_message *msgs;

	if (!db) { *out = NULL; return 0; }

	msgs = malloc(cap * sizeof(*msgs));
	if (!msgs) { *out = NULL; return 0; }

	const char *sql =
		"SELECT seq, role, content, llm_use FROM messages "
		"WHERE session_id = ? ORDER BY seq ASC;";

	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
		free(msgs);
		*out = NULL;
		return 0;
	}

	sqlite3_bind_int(stmt, 1, session_id);

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		if (count >= cap) {
			cap *= 2;
			struct db_message *tmp = realloc(msgs, cap * sizeof(*msgs));
			if (!tmp) { *out = msgs; sqlite3_finalize(stmt); return count; }
			msgs = tmp;
		}
		msgs[count].seq = sqlite3_column_int(stmt, 0);
		const char *r = (const char *)sqlite3_column_text(stmt, 1);
		const char *c = (const char *)sqlite3_column_text(stmt, 2);
		msgs[count].role = r ? strdup(r) : strdup("");
		msgs[count].content = c ? strdup(c) : strdup("");
		msgs[count].llm_use = sqlite3_column_int(stmt, 3);
		count++;
	}
	sqlite3_finalize(stmt);
	*out = msgs;
	return count;
}

void db_messages_free(struct db_message *msgs, int count)
{
	int i;
	if (!msgs) return;
	for (i = 0; i < count; i++) {
		free(msgs[i].role);
		free(msgs[i].content);
	}
	free(msgs);
}

/* --- settings --- */

char *db_setting_get(const char *key)
{
	sqlite3_stmt *stmt;
	char *result = NULL;

	if (!db) return NULL;

	if (sqlite3_prepare_v2(db,
		"SELECT value FROM settings WHERE key = ?;",
		-1, &stmt, NULL) != SQLITE_OK)
		return NULL;

	sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *val = (const char *)sqlite3_column_text(stmt, 0);
		if (val) result = strdup(val);
	}
	sqlite3_finalize(stmt);
	return result;
}

void db_setting_set(const char *key, const char *value)
{
	sqlite3_stmt *stmt;

	if (!db) return;

	if (sqlite3_prepare_v2(db,
		"INSERT INTO settings (key, value) VALUES (?, ?) "
		"ON CONFLICT(key) DO UPDATE SET value = excluded.value;",
		-1, &stmt, NULL) != SQLITE_OK)
		return;

	sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 2, value, -1, SQLITE_STATIC);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
}
