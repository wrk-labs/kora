#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <locale.h>
#ifdef __APPLE__
#include <ncurses.h>
#else
#include <ncursesw/ncurses.h>
#endif

#include "event.h"
#include "status.h"
#include "tui.h"

/*
 * Terminal UI for kora — ncurses-based chat window.
 *
 * Architecture: event-driven. Background threads never touch ncurses.
 * They push events onto a thread-safe queue. The main loop (inside
 * tui_input) drains events and renders at ~60fps.
 *
 * Layout:
 *   ┌──────────────────────────────────────────┐
 *   │  kora chat                   model-name  │  header
 *   ├──────────────────────────────────────────┤
 *   │                                          │
 *   │  **Hello, how are you?**   (bold=user)   │
 *   │                                          │
 *   │  I'm doing well!          (normal=asst)  │
 *   │                                          │  messages (scrolling)
 *   ├──────────────────────────────────────────┤
 *   │ > _                                      │  input box (3 lines)
 *   └──────────────────────────────────────────┘
 *    /help  /model  /compact  /clear  /exit       status bar (1 line)
 */

/* ncurses windows */
static WINDOW *win_header = NULL;
static WINDOW *win_chat = NULL;
static WINDOW *win_input = NULL;
static WINDOW *win_status = NULL;

/* color pairs */
#define CP_USER_MSG 1

/* state */
static char header_mode[64] = "kora";
static char header_model[128] = "";
static char *statusbar_text = NULL;
static char *statusbar_right_text = NULL;
static int spinner_active = 0;
static int initialized = 0;
static int user_msg_attr = A_BOLD;  /* fallback if no colors */

/* input line editing state */
static char *input_buf = NULL;
static size_t input_len = 0;
static size_t input_cap = 0;
static size_t input_pos = 0;  /* cursor position */

/* input history */
static char **input_history = NULL;
static int hist_count = 0;
static int hist_cap = 0;
static int hist_idx = 0;
static char *hist_saved = NULL;  /* saved current line when browsing history */

/* layout constants */
#define HEADER_H   3
#define INPUT_H    3
#define STATUS_H   1
#define BORDER_H   1

/* chat pad */
#define PAD_INITIAL_H 1000
static int pad_h = PAD_INITIAL_H;  /* current pad height */
static int chat_h = 0;             /* visible chat height */
static int chat_y = 0;             /* screen row where chat starts */
static int chat_cols = 0;          /* visible chat width */
static int chat_scroll = 0;        /* scroll offset (0 = bottom) */

/* poll interval for non-blocking input (ms) */
#define POLL_INTERVAL_MS 16

/* --- internal draw functions (main thread only) --- */

/* refresh the visible portion of the chat pad */
static void refresh_chat(void)
{
	int cur_y, cur_x;
	int top;

	if (!win_chat || chat_h <= 0) return;

	getyx(win_chat, cur_y, cur_x);
	(void)cur_x;

	/* top line of the pad to show */
	top = cur_y - chat_h + 1 - chat_scroll;
	if (top < 0) top = 0;

	pnoutrefresh(win_chat, top, 0, chat_y, 0,
	             chat_y + chat_h - 1, chat_cols - 1);
}

/* ensure the pad has room; grow if cursor is near the end */
static void chat_pad_ensure(void)
{
	int cur_y, cur_x;
	if (!win_chat) return;

	getyx(win_chat, cur_y, cur_x);
	(void)cur_x;

	if (cur_y >= pad_h - 10) {
		pad_h *= 2;
		wresize(win_chat, pad_h, chat_cols);
	}
}

/* auto-scroll to bottom (reset manual scroll) */
static void chat_scroll_bottom(void)
{
	chat_scroll = 0;
}

static void draw_statusbar(void)
{
	if (!win_status) return;

	int w = getmaxx(win_status);

	werase(win_status);
	wattron(win_status, A_DIM);

	if (statusbar_text)
		mvwprintw(win_status, 0, 2, "%s", statusbar_text);

	if (statusbar_right_text) {
		int rlen = (int)strlen(statusbar_right_text);
		int rx = w - rlen - 2;
		if (rx > 2)
			mvwprintw(win_status, 0, rx, "%s", statusbar_right_text);
	}

	wattroff(win_status, A_DIM);
	wnoutrefresh(win_status);
}

static void draw_header(void)
{
	int cols;
	int right_len;

	if (!win_header) return;

	cols = getmaxx(win_header);

	int mid = HEADER_H / 2;

	werase(win_header);
	wattron(win_header, A_BOLD);
	mvwprintw(win_header, mid, 2, "%s", header_mode);
	wattroff(win_header, A_BOLD);

	right_len = (int)strlen(header_model);
	if (right_len > 0) {
		wattron(win_header, A_DIM);
		mvwprintw(win_header, mid, cols - right_len - 2, "%s", header_model);
		wattroff(win_header, A_DIM);
	}

	wnoutrefresh(win_header);
}

static void draw_input_box(void)
{
	if (!win_input) return;

	werase(win_input);
	box(win_input, 0, 0);

	/* draw current input text */
	mvwprintw(win_input, 1, 2, "> ");
	if (input_buf && input_len > 0)
		mvwprintw(win_input, 1, 4, "%s", input_buf);

	/* position cursor — this window is always refreshed last
	   so the physical cursor stays here */
	wmove(win_input, 1, 4 + (int)input_pos);
	wrefresh(win_input);
}

static void layout_windows(void)
{
	int rows, cols;
	int input_y;

	getmaxyx(stdscr, rows, cols);
	/* header(1) + border(1) + chat + border(1) + input(3) + status(1) */
	chat_h = rows - HEADER_H - INPUT_H - STATUS_H - 2;
	if (chat_h < 1) chat_h = 1;
	chat_y = HEADER_H + BORDER_H;
	chat_cols = cols;

	if (win_header) delwin(win_header);
	if (win_input) delwin(win_input);
	if (win_status) delwin(win_status);

	win_header = newwin(HEADER_H, cols, 0, 0);

	/* chat pad: resize if it exists, create if it doesn't */
	if (win_chat) {
		wresize(win_chat, pad_h, cols);
	} else {
		if (pad_h < chat_h) pad_h = PAD_INITIAL_H;
		win_chat = newpad(pad_h, cols);
		scrollok(win_chat, TRUE);
	}

	input_y = chat_y + chat_h + BORDER_H;
	win_input = newwin(INPUT_H, cols, input_y, 0);
	win_status = newwin(STATUS_H, cols, input_y + INPUT_H, 0);

	keypad(win_input, TRUE);

	/* draw border lines on stdscr */
	attron(A_DIM);
	move(HEADER_H, 0);
	hline(ACS_HLINE, cols);
	move(HEADER_H + BORDER_H + chat_h, 0);
	hline(ACS_HLINE, cols);
	attroff(A_DIM);
	refresh();
}

/* --- internal statusbar helpers (main thread only) --- */

static void statusbar_set_left(const char *text)
{
	free(statusbar_text);
	statusbar_text = text ? strdup(text) : NULL;
	draw_statusbar();
	draw_input_box();
}

static void statusbar_set_right(const char *text)
{
	free(statusbar_right_text);
	statusbar_right_text = text ? strdup(text) : NULL;
	draw_statusbar();
	draw_input_box();
}

static const char *default_statusbar = "/help";

/* command table for dynamic statusbar hints */
static const char *slash_commands[] = {
	"/help", "/model", "/pull", "/compact", "/clear", "/exit", "/quit", NULL
};

static void update_dynamic_statusbar(void)
{
	if (!input_buf || input_len == 0) {
		statusbar_set_left(default_statusbar);
		return;
	}

	if (input_buf[0] != '/') {
		statusbar_set_left(NULL);
		return;
	}

	/* build list of matching commands */
	char matches[256];
	int pos = 0;
	int i;
	for (i = 0; slash_commands[i]; i++) {
		if (strncmp(slash_commands[i], input_buf, input_len) == 0) {
			int len = snprintf(matches + pos, sizeof(matches) - pos,
				"%s%s", pos > 0 ? "  " : "", slash_commands[i]);
			if (len > 0) pos += len;
		}
	}

	if (pos > 0)
		statusbar_set_left(matches);
	else
		statusbar_set_left(default_statusbar);
}

/* --- internal renderers (main thread only) --- */

/* print a line with background highlight filling the full row width */
static void print_highlighted_line(WINDOW *win, const char *text, int attr)
{
	int cols = getmaxx(win);
	int len, pad;
	char *line;

	/* "  text" with padding to fill full width */
	len = (int)strlen(text) + 2;  /* 2 for leading spaces */
	pad = len < cols ? cols - len : 0;

	line = malloc(len + pad + 1);
	if (!line) {
		wprintw(win, "  %s\n", text);
		return;
	}

	line[0] = ' ';
	line[1] = ' ';
	memcpy(line + 2, text, strlen(text));
	memset(line + len, ' ', pad);
	line[len + pad] = '\0';

	wattron(win, attr);
	wprintw(win, "%s", line);
	wattroff(win, attr);
	wprintw(win, "\n");
	free(line);
}

static void render_user_msg(const char *text)
{
	const char *p, *end;

	if (!win_chat) return;

	chat_pad_ensure();
	chat_scroll_bottom();

	wprintw(win_chat, "\n");

	/* handle multi-line input: highlight each line separately */
	p = text;
	while (*p) {
		end = strchr(p, '\n');
		if (end) {
			char *line = strndup(p, end - p);
			print_highlighted_line(win_chat, line ? line : p, user_msg_attr);
			free(line);
			p = end + 1;
		} else {
			print_highlighted_line(win_chat, p, user_msg_attr);
			break;
		}
	}
	refresh_chat();
}

static void render_assistant_begin(void)
{
	if (!win_chat) return;
	chat_pad_ensure();
	wprintw(win_chat, "  ");
	refresh_chat();
}

static void render_assistant_chunk(const char *text, int len)
{
	if (!win_chat) return;
	chat_pad_ensure();

	if (len > 0)
		waddnstr(win_chat, text, len);
	else
		wprintw(win_chat, "%s", text);
	refresh_chat();
}

static void render_assistant_end(void)
{
	if (!win_chat) return;
	chat_pad_ensure();
	wprintw(win_chat, "\n");
	chat_scroll_bottom();
	refresh_chat();
}

static void render_info(const char *text)
{
	if (!win_chat) return;
	chat_pad_ensure();
	chat_scroll_bottom();

	wattron(win_chat, A_DIM);
	wprintw(win_chat, "  %s\n", text);
	wattroff(win_chat, A_DIM);
	refresh_chat();
}

static void render_spinner(int on)
{
	spinner_active = on;
	if (!win_chat) return;

	if (on) {
		chat_pad_ensure();
		chat_scroll_bottom();
		wattron(win_chat, A_DIM);
		wprintw(win_chat, "  ...\n");
		wattroff(win_chat, A_DIM);
		refresh_chat();
	}
}

/* --- event drain (main thread only) --- */

static void drain_events(void)
{
	struct tui_event ev;
	int had_events = 0;
	int status_changed = 0;

	while (event_poll(&ev)) {
		had_events = 1;
		switch (ev.type) {
		case TUI_EV_INFO:
			render_info(ev.data ? ev.data : "");
			break;
		case TUI_EV_STATUSBAR_LEFT:
			statusbar_set_left(ev.data);
			break;
		case TUI_EV_CHAT_BEGIN:
			render_assistant_begin();
			break;
		case TUI_EV_CHAT_CHUNK:
			render_assistant_chunk(ev.data ? ev.data : "", ev.len);
			break;
		case TUI_EV_CHAT_END:
			render_assistant_end();
			break;
		case TUI_EV_USER_MSG:
			render_user_msg(ev.data ? ev.data : "");
			break;
		case TUI_EV_SPINNER:
			render_spinner(ev.len);
			break;
		default:
			break;
		}
		free(ev.data);
	}

	/* tick status handlers for right statusbar */
	const char *right = status_tick(&status_changed);
	if (status_changed)
		statusbar_set_right(right);

	/* restore cursor to input box after rendering */
	if (had_events || status_changed)
		draw_input_box();
}

/* --- input helpers --- */

static void input_buf_grow(size_t need)
{
	if (need + 1 <= input_cap)
		return;
	size_t new_cap = input_cap == 0 ? 128 : input_cap;
	while (new_cap <= need)
		new_cap *= 2;
	input_buf = realloc(input_buf, new_cap);
	input_cap = new_cap;
}

static void input_clear(void)
{
	input_len = 0;
	input_pos = 0;
	if (input_buf)
		input_buf[0] = '\0';
}

static void history_add(const char *line)
{
	if (!line || line[0] == '\0')
		return;
	/* skip duplicate of last entry */
	if (hist_count > 0 && strcmp(input_history[hist_count - 1], line) == 0)
		return;

	if (hist_count >= hist_cap) {
		int new_cap = hist_cap == 0 ? 64 : hist_cap * 2;
		input_history = realloc(input_history, new_cap * sizeof(char *));
		hist_cap = new_cap;
	}
	input_history[hist_count] = strdup(line);
	hist_count++;
}

static void input_set(const char *text)
{
	size_t len = strlen(text);
	input_buf_grow(len);
	memcpy(input_buf, text, len + 1);
	input_len = len;
	input_pos = len;
}

/* --- resize handler --- */

static void on_resize(int sig)
{
	(void)sig;
	endwin();
	refresh();
	layout_windows();
	draw_header();
	draw_statusbar();
	refresh_chat();
	draw_input_box();  /* must be last — owns the cursor */
}

/* --- public API: lifecycle --- */

void tui_get_size(int *rows, int *cols)
{
	int r, c;
	getmaxyx(stdscr, r, c);
	if (rows) *rows = r;
	if (cols) *cols = c;
}

void tui_init(void)
{
	setlocale(LC_ALL, "");

	event_init();

	initscr();
	cbreak();
	noecho();
	keypad(stdscr, TRUE);

	if (has_colors()) {
		start_color();
		use_default_colors();
		/* pick a dark gray background for user messages */
		if (COLORS >= 256) {
			/* 256-color: use 235 (#262626) — very subtle */
			init_pair(CP_USER_MSG, -1, 235);
			user_msg_attr = COLOR_PAIR(CP_USER_MSG);
		} else if (COLORS >= 16) {
			init_pair(CP_USER_MSG, -1, 8);
			user_msg_attr = COLOR_PAIR(CP_USER_MSG);
		}
	}

	curs_set(0);
	mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);
	mouseinterval(0);
	signal(SIGWINCH, on_resize);

	/* init input buffer */
	input_buf_grow(128);
	input_buf[0] = '\0';

	layout_windows();
	draw_header();
	statusbar_set_left(default_statusbar);
	draw_input_box();

	initialized = 1;
}

void tui_cleanup(void)
{
	int i;

	if (!initialized)
		return;

	if (win_header) { delwin(win_header); win_header = NULL; }
	if (win_chat) { delwin(win_chat); win_chat = NULL; }
	if (win_input) { delwin(win_input); win_input = NULL; }
	if (win_status) { delwin(win_status); win_status = NULL; }

	curs_set(1);
	endwin();

	/* free input state */
	free(input_buf);
	input_buf = NULL;
	input_cap = 0;
	input_len = 0;
	input_pos = 0;

	free(hist_saved);
	hist_saved = NULL;
	for (i = 0; i < hist_count; i++)
		free(input_history[i]);
	free(input_history);
	input_history = NULL;
	hist_count = 0;
	hist_cap = 0;

	free(statusbar_text);
	statusbar_text = NULL;
	free(statusbar_right_text);
	statusbar_right_text = NULL;

	event_cleanup();
	initialized = 0;
}

/* --- public API: direct (main thread only) --- */

void tui_set_header(const char *mode, const char *model)
{
	if (mode)
		snprintf(header_mode, sizeof(header_mode), "%s", mode);
	if (model)
		snprintf(header_model, sizeof(header_model), "%s", model);
	if (initialized) {
		draw_header();
		draw_input_box();
	}
}

void tui_set_status(const char *text)
{
	tui_statusbar(text);
}

void tui_draw(void)
{
	clear();
	refresh();
	layout_windows();
	draw_header();
	if (win_chat) {
		werase(win_chat);
		refresh_chat();
	}
	draw_statusbar();
	draw_input_box();  /* must be last — owns the cursor */
}

void tui_input_clear(void)
{
	input_clear();
	update_dynamic_statusbar();
}

int tui_permission(const char *prompt)
{
	int ch;

	if (!win_status) return 'n';

	/* show prompt in status bar */
	werase(win_status);
	wattron(win_status, A_BOLD);
	mvwprintw(win_status, 0, 1, "%s", prompt);
	wattroff(win_status, A_BOLD);
	wattron(win_status, A_DIM);
	wprintw(win_status, " [y/n/a] ");
	wattroff(win_status, A_DIM);
	wrefresh(win_status);

	/* wait for y/n/a keypress */
	curs_set(0);
	while (1) {
		ch = wgetch(win_status);
		if (ch == 'y' || ch == 'Y') { ch = 'y'; break; }
		if (ch == 'n' || ch == 'N') { ch = 'n'; break; }
		if (ch == 'a' || ch == 'A') { ch = 'a'; break; }
		if (ch == 27 || ch == 3)    { ch = 'n'; break; }  /* ESC or Ctrl-C = no */
	}

	/* restore normal statusbar */
	draw_statusbar();
	draw_input_box();
	return ch;
}

void tui_tool_call(const char *name, const char *args)
{
	if (!win_chat) return;
	chat_pad_ensure();
	chat_scroll_bottom();

	wattron(win_chat, A_DIM);
	wprintw(win_chat, "  [%s]", name);
	wattroff(win_chat, A_DIM);
	wprintw(win_chat, " %s\n", args ? args : "");
	refresh_chat();
	draw_input_box();
}

void tui_tool_result(const char *text, int display)
{
	const char *p, *end;

	if (!display || !win_chat)
		return;

	chat_pad_ensure();
	chat_scroll_bottom();

	p = text;
	wattron(win_chat, A_DIM);
	while (*p) {
		end = strchr(p, '\n');
		if (end) {
			wprintw(win_chat, "    %.*s\n", (int)(end - p), p);
			p = end + 1;
		} else {
			wprintw(win_chat, "    %s\n", p);
			break;
		}
	}
	wattroff(win_chat, A_DIM);
	refresh_chat();
	draw_input_box();
}

/* --- public API: event-based (safe from any thread) --- */

void tui_info(const char *text)
{
	struct tui_event ev = {0};
	ev.type = TUI_EV_INFO;
	ev.data = text ? strdup(text) : NULL;
	event_push(&ev);
	free(ev.data);
}

void tui_user_msg(const char *text)
{
	struct tui_event ev = {0};
	ev.type = TUI_EV_USER_MSG;
	ev.data = text ? strdup(text) : NULL;
	event_push(&ev);
	free(ev.data);
}

void tui_assistant_begin(void)
{
	struct tui_event ev = {0};
	ev.type = TUI_EV_CHAT_BEGIN;
	event_push(&ev);
}

void tui_assistant_chunk(const char *text, int len)
{
	struct tui_event ev = {0};
	ev.type = TUI_EV_CHAT_CHUNK;
	if (len > 0) {
		ev.data = malloc(len + 1);
		if (ev.data) {
			memcpy(ev.data, text, len);
			ev.data[len] = '\0';
		}
	} else {
		ev.data = text ? strdup(text) : NULL;
		len = ev.data ? (int)strlen(ev.data) : 0;
	}
	ev.len = len;
	event_push(&ev);
	free(ev.data);
}

void tui_assistant_end(void)
{
	struct tui_event ev = {0};
	ev.type = TUI_EV_CHAT_END;
	event_push(&ev);
}

void tui_statusbar(const char *text)
{
	struct tui_event ev = {0};
	ev.type = TUI_EV_STATUSBAR_LEFT;
	ev.data = text ? strdup(text) : NULL;
	event_push(&ev);
	free(ev.data);
}

void tui_spinner(int on)
{
	struct tui_event ev = {0};
	ev.type = TUI_EV_SPINNER;
	ev.len = on;
	event_push(&ev);
}

/* --- input loop (main thread, non-blocking with event drain) --- */

char *tui_input(const char *prompt)
{
	int ch;
	int max_visible;

	(void)prompt;

	if (!win_input) return NULL;

	input_clear();
	hist_idx = hist_count;
	free(hist_saved);
	hist_saved = NULL;

	curs_set(1);
	draw_input_box();
	update_dynamic_statusbar();

	/* non-blocking: poll input every POLL_INTERVAL_MS,
	   drain event queue between polls */
	wtimeout(win_input, POLL_INTERVAL_MS);

	while (1) {
		/* drain pending events from background threads */
		drain_events();

		ch = wgetch(win_input);

		if (ch == ERR)
			continue;  /* timeout, no input — loop and drain again */

		max_visible = getmaxx(win_input) - 6;  /* minus borders + "> " */
		if (max_visible < 1) max_visible = 1;

		switch (ch) {
		case '\n':
		case '\r':
		case KEY_ENTER:
			/* submit */
			wtimeout(win_input, -1);  /* restore blocking */
			curs_set(0);
			if (input_buf) {
				input_buf[input_len] = '\0';
				history_add(input_buf);
				return strdup(input_buf);
			}
			return strdup("");

		case 27:  /* ESC — could be escape key or alt sequence */
			/* for now, treat as cancel / empty return */
			wtimeout(win_input, -1);
			curs_set(0);
			return strdup("");

		case KEY_BACKSPACE:
		case 127:
		case 8:
			if (input_pos > 0) {
				memmove(input_buf + input_pos - 1,
					input_buf + input_pos,
					input_len - input_pos);
				input_pos--;
				input_len--;
				input_buf[input_len] = '\0';
				update_dynamic_statusbar();
			}
			break;

		case KEY_DC:  /* delete key */
			if (input_pos < input_len) {
				memmove(input_buf + input_pos,
					input_buf + input_pos + 1,
					input_len - input_pos - 1);
				input_len--;
				input_buf[input_len] = '\0';
				update_dynamic_statusbar();
			}
			break;

		case KEY_LEFT:
			if (input_pos > 0) {
				input_pos--;
				draw_input_box();
			}
			break;

		case KEY_RIGHT:
			if (input_pos < input_len) {
				input_pos++;
				draw_input_box();
			}
			break;

		case KEY_HOME:
		case 1:  /* Ctrl-A */
			input_pos = 0;
			draw_input_box();
			break;

		case KEY_END:
		case 5:  /* Ctrl-E */
			input_pos = input_len;
			draw_input_box();
			break;

		case KEY_UP:
			if (hist_count == 0) break;
			if (hist_idx == hist_count) {
				free(hist_saved);
				hist_saved = strdup(input_buf ? input_buf : "");
			}
			if (hist_idx > 0) {
				hist_idx--;
				input_set(input_history[hist_idx]);
				update_dynamic_statusbar();
			}
			break;

		case KEY_DOWN:
			if (hist_idx < hist_count) {
				hist_idx++;
				if (hist_idx == hist_count && hist_saved) {
					input_set(hist_saved);
				} else if (hist_idx < hist_count) {
					input_set(input_history[hist_idx]);
				}
				update_dynamic_statusbar();
			}
			break;

		case 21:  /* Ctrl-U: clear line */
			input_clear();
			update_dynamic_statusbar();
			break;

		case 23:  /* Ctrl-W: delete word backwards */
			if (input_pos > 0) {
				size_t old_pos = input_pos;
				while (input_pos > 0 && input_buf[input_pos - 1] == ' ')
					input_pos--;
				while (input_pos > 0 && input_buf[input_pos - 1] != ' ')
					input_pos--;
				memmove(input_buf + input_pos,
					input_buf + old_pos,
					input_len - old_pos);
				input_len -= (old_pos - input_pos);
				input_buf[input_len] = '\0';
				update_dynamic_statusbar();
			}
			break;

		case 4:  /* Ctrl-D on empty line = EOF */
			if (input_len == 0) {
				wtimeout(win_input, -1);
				curs_set(0);
				return NULL;
			}
			break;

		case 3:  /* Ctrl-C */
			wtimeout(win_input, -1);
			curs_set(0);
			return strdup("");

		case 12:  /* Ctrl-L: clear chat window */
			if (win_chat) {
				werase(win_chat);
				chat_scroll_bottom();
				refresh_chat();
				draw_input_box();
			}
			break;

		case KEY_PPAGE:  /* Page Up: scroll chat up */
			if (win_chat) {
				int cur_y, cur_x;
				getyx(win_chat, cur_y, cur_x);
				(void)cur_x;
				int max_scroll = cur_y - chat_h + 1;
				if (max_scroll < 0) max_scroll = 0;
				chat_scroll += chat_h / 2;
				if (chat_scroll > max_scroll)
					chat_scroll = max_scroll;
				refresh_chat();
				draw_input_box();
			}
			break;

		case KEY_NPAGE:  /* Page Down: scroll chat down */
			if (win_chat) {
				chat_scroll -= chat_h / 2;
				if (chat_scroll < 0)
					chat_scroll = 0;
				refresh_chat();
				draw_input_box();
			}
			break;

		case KEY_MOUSE:
			{
				MEVENT mev;
				if (getmouse(&mev) == OK && win_chat) {
					int cur_y2, cur_x2;
					getyx(win_chat, cur_y2, cur_x2);
					(void)cur_x2;
					int max_scroll2 = cur_y2 - chat_h + 1;
					if (max_scroll2 < 0) max_scroll2 = 0;

#ifdef BUTTON4_PRESSED
					if (mev.bstate & BUTTON4_PRESSED) {
						/* scroll up */
						chat_scroll += 3;
						if (chat_scroll > max_scroll2)
							chat_scroll = max_scroll2;
						refresh_chat();
						draw_input_box();
					}
#endif
#ifdef BUTTON5_PRESSED
					if (mev.bstate & BUTTON5_PRESSED) {
						/* scroll down */
						chat_scroll -= 3;
						if (chat_scroll < 0)
							chat_scroll = 0;
						refresh_chat();
						draw_input_box();
					}
#endif
				}
			}
			break;

		default:
			/* printable character */
			if (ch >= 32 && ch < 127) {
				input_buf_grow(input_len + 1);
				if (input_pos < input_len)
					memmove(input_buf + input_pos + 1,
						input_buf + input_pos,
						input_len - input_pos);
				input_buf[input_pos] = (char)ch;
				input_pos++;
				input_len++;
				input_buf[input_len] = '\0';
				update_dynamic_statusbar();
			}
			break;
		}
	}
}

/* --- welcome screen --- */

/* read and print the ASCII logo from file */
static void draw_logo(void)
{
	FILE *f = NULL;
	char line[256];

	/* try source tree first, then installed path */
	f = fopen("src/branding/ascii-logo.txt", "r");
	if (!f) {
		char path[512];
		snprintf(path, sizeof(path), "%s/branding/ascii-logo.txt", LUADIR);
		f = fopen(path, "r");
	}

	if (!f) return;

	wattron(win_chat, A_BOLD);
	while (fgets(line, sizeof(line), f))
		wprintw(win_chat, "  %s", line);
	wattroff(win_chat, A_BOLD);
	fclose(f);
}

void tui_draw_welcome(const char *model)
{
	if (!win_chat) return;

	wprintw(win_chat, "\n");
	draw_logo();
	wprintw(win_chat, "\n");

	wattron(win_chat, A_DIM);
	wprintw(win_chat, "  v%s", VERSION);
	if (model && model[0])
		wprintw(win_chat, " | %s", model);
	wprintw(win_chat, "\n");
	wprintw(win_chat, "  Type a message to start. /help for commands.\n");
	wattroff(win_chat, A_DIM);

	refresh_chat();
	doupdate();
}
