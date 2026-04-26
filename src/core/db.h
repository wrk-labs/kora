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
   callback receives: alias, filename, size, quant, downloaded, active,
   display_name (may be NULL), user_data */
typedef void (*db_model_cb)(const char *alias, const char *filename,
                            const char *size, const char *quant,
                            int downloaded, int active,
                            const char *display_name,
                            void *user_data);
void db_models_each(db_model_cb cb, void *user_data);

/* register a manually downloaded model (not in registry) */
void db_model_add(const char *filename);

/* set the active model */
void db_model_set_active(const char *name);

/* get the active model alias (or NULL). caller must NOT free. */
const char *db_model_get_active(void);

/* --- sessions --- */

/* create a new session, returns the session id (or -1 on error) */
int db_session_create(const char *mode, const char *model, const char *cwd);

/* update session name (e.g. after auto-naming) */
void db_session_set_name(int session_id, const char *name);

/* touch updated_at timestamp */
void db_session_touch(int session_id);

/* delete a session and its messages */
void db_session_delete(int session_id);

/* session info returned by list/get */
struct db_session {
	int id;
	char name[128];
	char mode[16];
	char model[128];
	char created_at[32];
	char updated_at[32];
	char last_message[256];
	int message_count;
};

/* list sessions (most recent first). returns count, fills out[] up to max.
   caller provides the array. */
int db_sessions_list(struct db_session *out, int max);

/* get a single session by id. returns 0 on success, -1 if not found. */
int db_session_get(int session_id, struct db_session *out);

/* --- messages --- */

/* append a message to a session. `model` is the alias of the model in effect
   for this turn (NULL ok for user/system messages or legacy callers). */
void db_message_add(int session_id, int seq, const char *role,
                    const char *content, const char *model, int llm_use);

/* update the status of an existing message ('ok' or 'failed'). used to
   roll back a user message when the daemon dispatch fails so the row
   isn't replayed in future context. */
void db_message_set_status(int session_id, int seq, const char *status);

/* message info returned by load */
struct db_message {
	int seq;
	char *role;
	char *content;
	char *model;   /* may be NULL */
	int llm_use;
	char *status;  /* 'ok' or 'failed' */
};

/* load all messages for a session. returns count, allocates *out.
   caller must free each role/content and the array itself. */
int db_messages_load(int session_id, struct db_message **out);

/* same as db_messages_load but excludes rows where status != 'ok'.
   use this when rehydrating an in-memory session for context, so failed
   user turns (daemon dispatch failures) aren't replayed to the model. */
int db_messages_load_for_context(int session_id, struct db_message **out);

/* free an array of messages returned by db_messages_load */
void db_messages_free(struct db_message *msgs, int count);

/* --- settings --- */

/* get a setting value, returns NULL if not found. caller must free. */
char *db_setting_get(const char *key);

/* set a setting value (upsert) */
void db_setting_set(const char *key, const char *value);

#endif
