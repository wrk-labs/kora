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
