#ifndef KORA_TUI_H
#define KORA_TUI_H

/* terminal UI for kora chat/code modes */

/* initialize TUI: enter alternate screen, setup signal handlers, raw mode */
void tui_init(void);

/* restore terminal and exit alternate screen */
void tui_cleanup(void);

/* set the header bar content (mode name, model name) */
void tui_set_header(const char *mode, const char *model);

/* set the status bar content (bottom line) */
void tui_set_status(const char *text);

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

/* print a system/info message (e.g. [Compressing context...]) */
void tui_info(const char *text);

/* print a tool call indicator: [tool_name] args */
void tui_tool_call(const char *name, const char *args);

/* print a tool result (optionally dimmed if display=false) */
void tui_tool_result(const char *text, int display);

/* show/hide a spinner on the status bar */
void tui_spinner(int on);

/* read a line of input with prompt, returns allocated string or NULL on EOF */
char *tui_input(const char *prompt);

/* clear and redraw the input box (e.g. after submitting a message) */
void tui_input_clear(void);

/* drain pending UI events (chunks, info, user/asst msgs) and render them.
   call from a synchronous foreground loop (e.g. the agent loop) so that
   queued events become visible without waiting for the next tui_input.
   safe to call from the main thread only. */
void tui_pump(void);

/* temporarily suspend ncurses so an external program (e.g. $EDITOR) can
   take over the terminal. call tui_resume() afterwards to reclaim it. */
void tui_suspend(void);
void tui_resume(void);

/* get terminal dimensions */
void tui_get_size(int *rows, int *cols);

/* draw welcome message after init */
void tui_draw_welcome(const char *model);

/* set the left status bar text (bottom line, below input box)
   shows shortcuts, mode info, or permission prompts */
void tui_statusbar(const char *text);

/* prompt the user for a yes/no/always/quit decision in the status bar.
   MAIN THREAD ONLY. blocks until the user answers. */
int tui_permission(const char *prompt);

/* same prompt, but callable from any thread. internally posts a request
   that the main thread (via drain_events / tui_input poll) services by
   calling tui_permission, then signals the caller. blocks the caller
   until an answer is available. */
int tui_request_permission(const char *prompt);

#endif
