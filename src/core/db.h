#ifndef KORA_DB_H
#define KORA_DB_H

#include <sqlite3.h>

/* open (or create) ~/.kora/kora.db — call once at startup */
int db_open(void);

/* close the database — call at shutdown */
void db_close(void);

/* raw handle for direct queries if needed */
sqlite3 *db_handle(void);

/* --- models --- */

/* sync the registry table with the compiled-in registry[].
   marks models as downloaded if the file exists on disk. */
void db_models_sync(void);

/* check if a model (by alias or filename) is downloaded */
int db_model_is_downloaded(const char *name);

/* mark a model as downloaded (after a successful pull) */
void db_model_set_downloaded(const char *name, int downloaded);

/* iterate all known models (registry + manually added).
   callback receives: alias, filename, size, quant, downloaded, active, user_data */
typedef void (*db_model_cb)(const char *alias, const char *filename,
                            const char *size, const char *quant,
                            int downloaded, int active, void *user_data);
void db_models_each(db_model_cb cb, void *user_data);

/* register a manually downloaded model (not in registry) */
void db_model_add(const char *filename);

/* set the active model */
void db_model_set_active(const char *name);

/* get the active model alias (or NULL). caller must NOT free. */
const char *db_model_get_active(void);

/* --- settings --- */

/* get a setting value, returns NULL if not found. caller must free. */
char *db_setting_get(const char *key);

/* set a setting value (upsert) */
void db_setting_set(const char *key, const char *value);

#endif
