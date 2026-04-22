#ifndef KORA_TUI_H
#define KORA_TUI_H

struct db_session;

/* terminal UI for kora chat */

/* initialize TUI: enter alternate screen, setup signal handlers, raw mode */
void tui_init(void);

/* restore terminal and exit alternate screen */
void tui_cleanup(void);

/* set the header bar content (mode name, model name) */
void tui_set_header(const char *mode, const char *model);

/* full redraw of the screen */
void tui_draw(void);


/* append a user message to the message area */
void tui_user_msg(const char *text);

/* begin an assistant message (prints the "kora" label) */
void tui_assistant_begin(void);

/* stream a chunk of assistant text (called per token) */
void tui_assistant_chunk(const char *text, int len);

/* end the current assistant message (finalize + newline) */
void tui_assistant_end(void);

/* print a system/info message (e.g. /help output) — lands in the chat
   pad. use sparingly; prefer tui_log for operational events so the chat
   stays focused on user ↔ assistant turns. */
void tui_info(const char *text);

/* append a line to the operational-log pane below MODELS. thread-safe:
   background workers can call this directly. messages are prefixed with
   a short wall-clock timestamp, displayed dim, newest at the bottom.
   the ring buffer holds the last ~500 entries; older ones drop off. */
void tui_log(const char *fmt, ...)
#ifdef __GNUC__
	__attribute__((format(printf, 1, 2)))
#endif
	;

/* read a line of input with prompt, returns allocated string or NULL on EOF */
char *tui_input(const char *prompt);

/* clear and redraw the input box (e.g. after submitting a message) */
void tui_input_clear(void);

/* get terminal dimensions */
void tui_get_size(int *rows, int *cols);

/* draw welcome message after init */
void tui_draw_welcome(const char *model);

/* clear the chat panel (used after deleting the current session) */
void tui_clear_chat(void);

/* populate the left session-list pane. the list is deep-copied; caller may
   free after return. active_id is the DB id of the currently-open session
   (or -1 if none) — that entry gets the active-row marker. */
void tui_set_sessions(const struct db_session *list, int n, int active_id);

/* set the displayed session name in the header bar (right of the mode). */
void tui_set_session_name(const char *name);

/* update the daemon-status dot in the header (1 = up, 0 = down).
   MAIN THREAD ONLY (touches ncurses directly). */
void tui_set_daemon_status(int up);

/* thread-safe variant: enqueue a status change. the main loop will apply it
   on its next tick. call this from background pollers / signal handlers. */
void tui_post_daemon_status(int up);

/* flag that a cancellable background op is in flight (generation, compact).
   when set, Esc pressed in CHAT returns an empty submit so main.c's
   cancellation path runs instead of the focus-switch fallback. safe from
   any thread (single-byte flag). */
void tui_set_cancellable(int yn);

/* thread-safe: tells the TUI a model just finished pulling / was removed.
   the main loop calls the registered refresh callback on its next tick. */
void tui_post_models_refresh(void);
void tui_set_models_refresh_cb(void (*cb)(void));

/* right-side admin pane: list of models the user can select / operate on.
   `alias` is the internal identifier (stable, passed to the daemon). when
   `display` is non-empty, the pane renders that string instead of alias
   — used for manual pulls where the GGUF `general.name` is nicer to look
   at than the filename-derived alias. */
struct tui_model_row {
	char alias[64];
	char display[96];
	char size[16];
	char quant[16];
	int  downloaded;
	int  is_current;
};

/* populate the right admin pane. the list is deep-copied. */
void tui_set_models(const struct tui_model_row *list, int n);

/* thread-safe: mark an alias as currently-downloading (shown with a ⟳
   glyph in the MODELS pane). pass NULL to clear. no-op if the pane is
   not visible. safe to call from background worker threads. */
void tui_set_downloading_alias(const char *alias);

/* show / hide the "↑ queued: <preview>" indicator on the horizontal rule
   directly above the input box. pass NULL or empty to hide. main thread
   only — callers are already on the main thread when toggling queue state. */
void tui_set_queued(const char *preview);

/* thread-safe: tell tui_input to return as soon as the input buffer is empty.
   used by background generation workers after completing a turn so the main
   loop can process a queued follow-up message without waiting for the user
   to press a key. if the user is mid-edit (buffer non-empty), the wake is
   suppressed so we don't discard their in-progress line — they can submit
   it first and the queue fires on the next idle moment. when the wake does
   fire, tui_input returns the literal string "\x02wake" which main.c
   recognises. */
void tui_post_wake(void);

/* --- accessors for pane-initiated commands (main.c reads these to know
       which session/model the user is acting on) --- */

/* DB id of the currently-highlighted session row, or -1 if none. */
int tui_highlighted_session_id(void);

/* alias of the currently-highlighted model, or NULL if none.
   points to internal storage; do not free. valid until the next
   tui_set_models or tui_cleanup. */
const char *tui_highlighted_model_alias(void);

/* programmatically move focus. used by main.c after pane-initiated
   actions to return the user to the natural next surface (e.g. CHAT
   after /new, SESSIONS after /delete). */
void tui_focus_chat(void);
void tui_focus_sessions(void);
void tui_focus_admin(void);

/* set the left status bar text (bottom line, below input box)
   shows shortcuts, mode info, or permission prompts */
void tui_statusbar(const char *text);

/* prompt the user for a yes/no/always/quit decision in the status bar.
   MAIN THREAD ONLY. blocks until the user answers. */
int tui_permission(const char *prompt);

#endif
