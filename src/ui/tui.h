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

/* get terminal dimensions */
void tui_get_size(int *rows, int *cols);

/* draw welcome message after init */
void tui_draw_welcome(const char *model);

/* set the left status bar text (bottom line, below input box)
   shows shortcuts, mode info, or permission prompts */
void tui_statusbar(const char *text);

/* prompt the user for a yes/no/always decision in the status bar
   returns 'y', 'n', or 'a' */
int tui_permission(const char *prompt);

#endif
