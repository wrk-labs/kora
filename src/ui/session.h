#ifndef KORA_SESSION_H
#define KORA_SESSION_H

struct kora_message;

/* A chat session — conversation state for one logical conversation.
   Holds message history + persistence metadata + per-session config.
   For now there is only ever one active session in the TUI; stage 5.4
   will extend to multiple. */
struct kora_session {
	int   db_id;                 /* sessions table id; -1 if not persisted */
	char  name[128];             /* displayable name (from DB, may be empty) */
	char *model;                 /* alias; NULL = use global default */
	char *system_prompt;         /* NULL = no system message */

	/* message history (all owned; deep-copied) */
	char **roles;
	char **contents;
	int    n_msg;
	int    cap_msg;

	/* persistence bookkeeping */
	int msg_seq;                 /* next seq for db_message_add */
	int user_msg_count;          /* used to trigger auto-naming at 10 */
	int named;                   /* 1 once auto-named */
};

/* Create a session. model may be NULL; system_prompt may be NULL (no system
   message) or a string (deep-copied and inserted as message[0]). */
struct kora_session *kora_session_new(const char *model, const char *system_prompt);

void kora_session_free(struct kora_session *s);

/* Append a message (deep-copy). Returns 0 on success, -1 on OOM. */
int kora_session_add(struct kora_session *s, const char *role, const char *content);

/* Drop all messages except the system prompt (if present as message[0]). */
void kora_session_clear(struct kora_session *s);

/* Swap the model. Strdups; old value freed. Pass NULL to clear override. */
int kora_session_set_model(struct kora_session *s, const char *model);

/* Cheap chars/4 approximation for context-gauge display. */
int kora_session_approx_tokens(const struct kora_session *s);

/* Deep-copy messages into a newly-allocated kora_message[]. Returns count
   (>= 0) or -1 on OOM. Caller must free via kora_session_snapshot_free. */
int  kora_session_snapshot(const struct kora_session *s, struct kora_message **out);
void kora_session_snapshot_free(struct kora_message *msgs, int n);

/* Build a plain "role: content\n…" transcript of non-system messages for
   use as the user-side payload to a compact/naming request. Returns a
   caller-owned string, or NULL on OOM. */
char *kora_session_transcript(const struct kora_session *s);

#endif
