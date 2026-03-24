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
#define INPUT_H_MIN 3
#define INPUT_H_MAX 12
#define STATUS_H   1
#define BORDER_H   1

/* multiline input state */
static int input_h = INPUT_H_MIN;
static int input_scroll = 0;  /* first visible line in multiline input */

/* chat pad */
#define PAD_INITIAL_H 1000
static int pad_h = PAD_INITIAL_H;  /* current pad height */
static int chat_h = 0;             /* visible chat height */
static int chat_y = 0;             /* screen row where chat starts */
static int chat_cols = 0;          /* visible chat width */
static int chat_scroll = 0;        /* scroll offset (0 = bottom) */

/* poll interval for non-blocking input (ms) */
#define POLL_INTERVAL_MS 16

/* forward declarations for multiline input helpers */
static int input_cur_line(void);
static size_t input_line_start_pos(int line);
static size_t input_line_end_pos(int line);
static void recalc_input_height(void);

/* forward declarations for visual (soft-wrap) helpers */
static int input_content_width(void);
static int input_visual_nlines(void);
static void input_pos_to_visual(size_t pos, int *vrow, int *vcol);
static size_t input_visual_to_pos(int target_vrow, int target_vcol);

/* --- paste slot storage --- */

/*
 * Pasted content is stored in slots. Each slot is referenced by a single
 * sentinel byte in input_buf (0x01 = slot 0, 0x02 = slot 1, ...).
 * The sentinel is atomic: backspace/delete removes the whole byte,
 * which frees the stored content. On submit, sentinels are expanded
 * back to full text. No partial deletion or malformed state is possible.
 */
#define PASTE_MARKER_BASE 0x01
#define MAX_PASTE_SLOTS   26
#define PASTE_COLLAPSE_LINES 3
#define PASTE_COLLAPSE_CHARS 150

struct paste_slot {
	char *content;
	int nlines;
};

static struct paste_slot paste_slots[MAX_PASTE_SLOTS];
static int paste_count = 0;

static int is_paste_marker(char c)
{
	unsigned char uc = (unsigned char)c;
	return uc >= PASTE_MARKER_BASE
	    && uc < PASTE_MARKER_BASE + MAX_PASTE_SLOTS
	    && (int)(uc - PASTE_MARKER_BASE) < paste_count;
}

static int paste_slot_of(char c)
{
	return (unsigned char)c - PASTE_MARKER_BASE;
}

/* get badge display text for a paste slot; returns length */
static int paste_badge(int slot, char *buf, int bufsz)
{
	if (slot < 0 || slot >= paste_count)
		return snprintf(buf, bufsz, "[Pasted]");
	return snprintf(buf, bufsz, "[Pasted ~%d lines]",
			paste_slots[slot].nlines);
}

/* visual width of a character in the input buffer */
static int input_char_width(char c)
{
	if (is_paste_marker(c)) {
		char tmp[32];
		return paste_badge(paste_slot_of(c), tmp, sizeof(tmp));
	}
	return 1;
}

/* free a paste slot's content */
static void paste_slot_free(int slot)
{
	if (slot >= 0 && slot < paste_count) {
		free(paste_slots[slot].content);
		paste_slots[slot].content = NULL;
		paste_slots[slot].nlines = 0;
	}
}

/* free all paste slots */
static void paste_slots_clear(void)
{
	int i;
	for (i = 0; i < paste_count; i++) {
		free(paste_slots[i].content);
		paste_slots[i].content = NULL;
		paste_slots[i].nlines = 0;
	}
	paste_count = 0;
}

/* count lines in a string */
static int count_lines(const char *s)
{
	int n = 1;
	for (; *s; s++)
		if (*s == '\n') n++;
	return n;
}

/* expand all paste markers in a string, returns new allocated string */
static char *paste_expand(const char *src, size_t len)
{
	/* first pass: compute total size */
	size_t total = 0;
	size_t i;
	for (i = 0; i < len; i++) {
		if (is_paste_marker(src[i])) {
			int slot = paste_slot_of(src[i]);
			if (paste_slots[slot].content)
				total += strlen(paste_slots[slot].content);
		} else {
			total++;
		}
	}

	char *out = malloc(total + 1);
	if (!out) return strdup(src);

	size_t pos = 0;
	for (i = 0; i < len; i++) {
		if (is_paste_marker(src[i])) {
			int slot = paste_slot_of(src[i]);
			if (paste_slots[slot].content) {
				size_t clen = strlen(paste_slots[slot].content);
				memcpy(out + pos, paste_slots[slot].content, clen);
				pos += clen;
			}
		} else {
			out[pos++] = src[i];
		}
	}
	out[pos] = '\0';
	return out;
}

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
	int vis_lines, w;
	int cur_vrow, cur_vcol;
	int vl, col;
	size_t i;
	int needs_prefix;

	if (!win_input) return;

	vis_lines = input_h - 2; /* visible content rows */
	w = input_content_width();
	if (w < 1) w = 1;

	input_pos_to_visual(input_pos, &cur_vrow, &cur_vcol);

	/* adjust scroll to keep cursor visible */
	if (cur_vrow < input_scroll)
		input_scroll = cur_vrow;
	if (cur_vrow >= input_scroll + vis_lines)
		input_scroll = cur_vrow - vis_lines + 1;

	werase(win_input);
	box(win_input, 0, 0);

	/* render buffer with soft wrapping and paste badges */
	vl = 0;
	col = 0;
	needs_prefix = 1;

	for (i = 0; i <= input_len; i++) {
		if (vl >= input_scroll + vis_lines)
			break;

		/* draw prefix at start of each visible visual line */
		if (needs_prefix && vl >= input_scroll) {
			int row = 1 + (vl - input_scroll);
			if (vl == 0)
				mvwprintw(win_input, row, 2, "> ");
			else
				mvwprintw(win_input, row, 2, "  ");
			needs_prefix = 0;
		}

		if (i == input_len)
			break;

		if (input_buf[i] == '\n') {
			vl++;
			col = 0;
			needs_prefix = 1;
			continue;
		}

		if (is_paste_marker(input_buf[i])) {
			/* draw paste badge */
			char badge[32];
			int blen = paste_badge(paste_slot_of(input_buf[i]),
					       badge, sizeof(badge));
			int j;
			for (j = 0; j < blen; j++) {
				if (col >= w) {
					vl++;
					col = 0;
					needs_prefix = 1;
					if (vl >= input_scroll + vis_lines)
						break;
					if (needs_prefix && vl >= input_scroll) {
						int row = 1 + (vl - input_scroll);
						mvwprintw(win_input, row, 2, "  ");
						needs_prefix = 0;
					}
				}
				if (vl >= input_scroll) {
					int row = 1 + (vl - input_scroll);
					wattron(win_input, A_DIM);
					mvwaddch(win_input, row, 4 + col, badge[j]);
					wattroff(win_input, A_DIM);
				}
				col++;
			}
			continue;
		}

		/* draw regular character if on a visible visual line */
		if (vl >= input_scroll) {
			int row = 1 + (vl - input_scroll);
			mvwaddch(win_input, row, 4 + col, input_buf[i]);
		}

		col++;
		if (col >= w) {
			vl++;
			col = 0;
			needs_prefix = 1;
		}
	}

	/* position cursor */
	wmove(win_input, 1 + (cur_vrow - input_scroll), 4 + cur_vcol);
	wrefresh(win_input);
}

static void layout_windows(void)
{
	int rows, cols;
	int input_y;

	getmaxyx(stdscr, rows, cols);
	/* header(3) + border(1) + chat + border(1) + input(dynamic) + status(1) */
	chat_h = rows - HEADER_H - input_h - STATUS_H - 2;
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
	win_input = newwin(input_h, cols, input_y, 0);
	win_status = newwin(STATUS_H, cols, input_y + input_h, 0);

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

static const char *default_statusbar = "/help  Alt+Enter: new line";

/* command table for dynamic statusbar hints */
static const char *slash_commands[] = {
	"/help", "/model", "/pull", "/compact", "/clear",
	"/resume", "/exit", "/quit", NULL
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
static void render_user_msg(const char *text)
{
	int cols;
	const char *p, *end;
	int tlen, pad;

	if (!win_chat) return;

	chat_pad_ensure();
	chat_scroll_bottom();
	cols = getmaxx(win_chat);

	wprintw(win_chat, "\n");

	/* render entire message as a single highlighted block.
	   each line gets 2-space indent and is padded to full width
	   so the background highlight is continuous (no gaps). */
	wattron(win_chat, user_msg_attr);
	p = text;
	while (*p) {
		end = strchr(p, '\n');
		tlen = end ? (int)(end - p) : (int)strlen(p);
		pad = cols - tlen - 2;
		if (pad < 0) pad = 0;

		wprintw(win_chat, "  %.*s%*s", tlen, p, pad, "");

		if (end)
			p = end + 1;
		else
			break;
	}
	wattroff(win_chat, user_msg_attr);
	wprintw(win_chat, "\n");
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
	paste_slots_clear();
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

/* --- multiline input helpers --- */

/* which logical line (0-based) the cursor is on */
static int input_cur_line(void)
{
	int line = 0;
	size_t i;
	for (i = 0; i < input_pos; i++)
		if (input_buf[i] == '\n') line++;
	return line;
}

/* start position of logical line n (0-based) */
static size_t input_line_start_pos(int line)
{
	size_t i;
	int cur = 0;
	if (line <= 0) return 0;
	for (i = 0; i < input_len; i++) {
		if (input_buf[i] == '\n') {
			cur++;
			if (cur == line) return i + 1;
		}
	}
	return input_len;
}

/* end position of line n (position of '\n' or input_len) */
static size_t input_line_end_pos(int line)
{
	size_t start = input_line_start_pos(line);
	size_t i;
	for (i = start; i < input_len; i++) {
		if (input_buf[i] == '\n') return i;
	}
	return input_len;
}

/* insert a character at cursor position */
static void input_insert_char(char c)
{
	input_buf_grow(input_len + 1);
	if (input_pos < input_len)
		memmove(input_buf + input_pos + 1,
			input_buf + input_pos,
			input_len - input_pos);
	input_buf[input_pos] = c;
	input_pos++;
	input_len++;
	input_buf[input_len] = '\0';
}

/* --- visual (soft-wrap aware) helpers --- */

/* available text width per row inside the input box */
static int input_content_width(void)
{
	if (!win_input) return 40;
	/* cols - left_border(1) - prefix_space(1) - "> "(2) - right_border(1) */
	int w = getmaxx(win_input) - 5;
	return w > 1 ? w : 1;
}

/* advance visual column by cw chars, handling soft wrapping */
static void advance_col(int cw, int w, int *vl, int *col)
{
	*col += cw;
	while (*col >= w) {
		(*vl)++;
		*col -= w;
	}
}

/* count total visual lines considering soft wrapping and paste badges */
static int input_visual_nlines(void)
{
	int w = input_content_width();
	int vlines = 1;
	int col = 0;
	size_t i;

	for (i = 0; i < input_len; i++) {
		if (input_buf[i] == '\n') {
			vlines++;
			col = 0;
		} else {
			int cw = input_char_width(input_buf[i]);
			col += cw;
			while (col >= w) {
				vlines++;
				col -= w;
			}
		}
	}
	return vlines;
}

/* convert buffer position to visual (row, col) accounting for
   soft wrapping and paste badges */
static void input_pos_to_visual(size_t pos, int *vrow, int *vcol)
{
	int w = input_content_width();
	int r = 0, c = 0;
	size_t i;

	for (i = 0; i < pos && i < input_len; i++) {
		if (input_buf[i] == '\n') {
			r++;
			c = 0;
		} else {
			advance_col(input_char_width(input_buf[i]), w, &r, &c);
		}
	}
	*vrow = r;
	*vcol = c;
}

/* convert visual (row, col) to buffer position (clamps col to line end).
   paste markers are atomic — if target falls inside a badge, snaps to
   the marker position */
static size_t input_visual_to_pos(int target_vrow, int target_vcol)
{
	int w = input_content_width();
	int vl = 0, col = 0;
	size_t i;
	size_t last_on_row = 0;
	int on_target = 0;

	for (i = 0; i < input_len; i++) {
		if (vl == target_vrow) {
			on_target = 1;
			if (col >= target_vcol)
				return i;
			last_on_row = i;
		} else if (on_target) {
			return last_on_row;
		}

		if (input_buf[i] == '\n') {
			vl++;
			col = 0;
		} else {
			int cw = input_char_width(input_buf[i]);
			int old_vl = vl;
			advance_col(cw, w, &vl, &col);
			/* if we wrapped past the target row, clamp */
			if (old_vl == target_vrow && vl > target_vrow && on_target)
				return i;
		}
	}

	if (vl == target_vrow)
		return input_len;
	if (on_target)
		return last_on_row;
	return input_len;
}

/* recalculate input box height based on visual line count */
static void recalc_input_height(void)
{
	int rows, cols;
	int nlines = input_visual_nlines();
	int new_h = nlines + 2; /* border + lines + border */
	int max_h;

	getmaxyx(stdscr, rows, cols);
	(void)cols;
	max_h = rows / 2;
	if (max_h < INPUT_H_MIN) max_h = INPUT_H_MIN;
	if (max_h > INPUT_H_MAX) max_h = INPUT_H_MAX;

	if (new_h < INPUT_H_MIN) new_h = INPUT_H_MIN;
	if (new_h > max_h) new_h = max_h;

	if (new_h != input_h) {
		input_h = new_h;
		layout_windows();
		draw_header();
		refresh_chat();
		draw_statusbar();
	}
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

	/* enable bracketed paste mode */
	printf("\033[?2004h");
	fflush(stdout);

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

	/* disable bracketed paste mode */
	printf("\033[?2004l");
	fflush(stdout);

	curs_set(1);
	endwin();

	/* free input state */
	free(input_buf);
	input_buf = NULL;
	input_cap = 0;
	input_len = 0;
	input_pos = 0;
	paste_slots_clear();

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
	input_scroll = 0;
	if (input_h != INPUT_H_MIN) {
		input_h = INPUT_H_MIN;
		layout_windows();
		draw_header();
		refresh_chat();
		draw_statusbar();
	}
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

		switch (ch) {
		case '\n':
		case '\r':
		case KEY_ENTER:
			/* submit */
			input_scroll = 0;
			if (input_h != INPUT_H_MIN) {
				input_h = INPUT_H_MIN;
				layout_windows();
				draw_header();
				refresh_chat();
				draw_statusbar();
			}
			wtimeout(win_input, -1);  /* restore blocking */
			curs_set(0);
			if (input_buf) {
				char *result;
				input_buf[input_len] = '\0';
				/* expand paste markers to full content */
				if (paste_count > 0)
					result = paste_expand(input_buf,
							      input_len);
				else
					result = strdup(input_buf);
				history_add(result);
				paste_slots_clear();
				return result;
			}
			return strdup("");

		case 27: { /* ESC — could be escape key, Alt+key, or paste sequence */
			int next;
			wtimeout(win_input, 50);
			next = wgetch(win_input);
			wtimeout(win_input, POLL_INTERVAL_MS);

			if (next == '\n' || next == '\r' || next == KEY_ENTER) {
				/* Alt+Enter: insert newline */
				input_insert_char('\n');
				recalc_input_height();
				update_dynamic_statusbar();
				break;
			}

			if (next == '[') {
				/* could be bracketed paste: ESC [ 2 0 0 ~ */
				int seq[4];
				int si = 0;
				wtimeout(win_input, 50);
				for (si = 0; si < 4; si++) {
					seq[si] = wgetch(win_input);
					if (seq[si] == ERR) break;
				}
				wtimeout(win_input, POLL_INTERVAL_MS);

				if (si == 4 && seq[0] == '2' && seq[1] == '0'
				    && seq[2] == '0' && seq[3] == '~') {
					/* bracketed paste — collect all content first */
					size_t pcap = 4096, plen = 0;
					char *pbuf = malloc(pcap);
					int pc, end_state = 0;

					wtimeout(win_input, 100);
					while (pbuf) {
						pc = wgetch(win_input);
						if (pc == ERR) break;

						/* detect ESC[201~ end sequence */
						switch (end_state) {
						case 0: end_state = (pc == 27) ? 1 : 0; break;
						case 1: end_state = (pc == '[') ? 2 : 0; break;
						case 2: end_state = (pc == '2') ? 3 : 0; break;
						case 3: end_state = (pc == '0') ? 4 : 0; break;
						case 4: end_state = (pc == '1') ? 5 : 0; break;
						case 5: end_state = (pc == '~') ? 6 : 0; break;
						}
						if (end_state == 6) break;

						if (end_state == 0) {
							if (plen + 2 >= pcap) {
								pcap *= 2;
								char *nb = realloc(pbuf, pcap);
								if (!nb) break;
								pbuf = nb;
							}
							if (pc == '\n' || pc == '\r')
								pbuf[plen++] = '\n';
							else if (pc >= 32 && pc < 127)
								pbuf[plen++] = (char)pc;
							else if (pc == '\t')
								pbuf[plen++] = '\t';
						}
					}
					wtimeout(win_input, POLL_INTERVAL_MS);

					if (pbuf) {
						pbuf[plen] = '\0';
						int nlines = count_lines(pbuf);

						if ((nlines >= PASTE_COLLAPSE_LINES
						     || (int)plen > PASTE_COLLAPSE_CHARS)
						    && paste_count < MAX_PASTE_SLOTS) {
							/* collapse into a paste slot */
							int slot = paste_count++;
							paste_slots[slot].content = pbuf;
							paste_slots[slot].nlines = nlines;
							input_insert_char(
								(char)(PASTE_MARKER_BASE + slot));
						} else {
							/* small paste — insert inline */
							size_t pi;
							for (pi = 0; pi < plen; pi++)
								input_insert_char(pbuf[pi]);
							free(pbuf);
						}
					}

					recalc_input_height();
					update_dynamic_statusbar();
					draw_input_box();
				}
				/* else: unknown CSI sequence, ignore */
				break;
			}

			if (next == ERR) {
				/* plain ESC: cancel */
				input_scroll = 0;
				if (input_h != INPUT_H_MIN) {
					input_h = INPUT_H_MIN;
					layout_windows();
					draw_header();
					refresh_chat();
					draw_statusbar();
				}
				wtimeout(win_input, -1);
				curs_set(0);
				return strdup("");
			}
			/* other Alt+key: ignore */
			break;
		}

		case KEY_BACKSPACE:
		case 127:
		case 8:
			if (input_pos > 0) {
				if (is_paste_marker(input_buf[input_pos - 1]))
					paste_slot_free(paste_slot_of(
						input_buf[input_pos - 1]));
				memmove(input_buf + input_pos - 1,
					input_buf + input_pos,
					input_len - input_pos);
				input_pos--;
				input_len--;
				input_buf[input_len] = '\0';
				recalc_input_height();
				update_dynamic_statusbar();
			}
			break;

		case KEY_DC:  /* delete key */
			if (input_pos < input_len) {
				if (is_paste_marker(input_buf[input_pos]))
					paste_slot_free(paste_slot_of(
						input_buf[input_pos]));
				memmove(input_buf + input_pos,
					input_buf + input_pos + 1,
					input_len - input_pos - 1);
				input_len--;
				input_buf[input_len] = '\0';
				recalc_input_height();
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
		case 1:  /* Ctrl-A: start of current line */
			input_pos = input_line_start_pos(input_cur_line());
			draw_input_box();
			break;

		case KEY_END:
		case 5:  /* Ctrl-E: end of current line */
			input_pos = input_line_end_pos(input_cur_line());
			draw_input_box();
			break;

		case KEY_UP: {
			int vrow, vcol;
			input_pos_to_visual(input_pos, &vrow, &vcol);
			if (vrow > 0) {
				/* move cursor up one visual row */
				input_pos = input_visual_to_pos(vrow - 1, vcol);
			} else {
				/* first visual row: browse history */
				if (hist_count == 0) break;
				if (hist_idx == hist_count) {
					free(hist_saved);
					hist_saved = strdup(input_buf ? input_buf : "");
				}
				if (hist_idx > 0) {
					hist_idx--;
					input_set(input_history[hist_idx]);
					recalc_input_height();
				}
			}
			update_dynamic_statusbar();
			break;
		}

		case KEY_DOWN: {
			int vrow, vcol;
			int total_vlines = input_visual_nlines();
			input_pos_to_visual(input_pos, &vrow, &vcol);
			if (vrow < total_vlines - 1) {
				/* move cursor down one visual row */
				input_pos = input_visual_to_pos(vrow + 1, vcol);
			} else {
				/* last visual row: browse history */
				if (hist_idx < hist_count) {
					hist_idx++;
					if (hist_idx == hist_count && hist_saved) {
						input_set(hist_saved);
					} else if (hist_idx < hist_count) {
						input_set(input_history[hist_idx]);
					}
					recalc_input_height();
				}
			}
			update_dynamic_statusbar();
			break;
		}

		case 21:  /* Ctrl-U: clear line */
			input_clear();
			input_scroll = 0;
			recalc_input_height();
			update_dynamic_statusbar();
			break;

		case 23:  /* Ctrl-W: delete word backwards (stops at newlines and paste markers) */
			if (input_pos > 0) {
				size_t old_pos = input_pos;
				size_t di;
				/* if immediately before a paste marker, just delete it */
				if (is_paste_marker(input_buf[input_pos - 1])) {
					paste_slot_free(paste_slot_of(
						input_buf[input_pos - 1]));
					input_pos--;
				} else {
					while (input_pos > 0
					       && input_buf[input_pos - 1] == ' ')
						input_pos--;
					while (input_pos > 0
					       && input_buf[input_pos - 1] != ' '
					       && input_buf[input_pos - 1] != '\n'
					       && !is_paste_marker(
							input_buf[input_pos - 1]))
						input_pos--;
				}
				/* free any paste markers in the deleted range */
				for (di = input_pos; di < old_pos; di++) {
					if (is_paste_marker(input_buf[di]))
						paste_slot_free(
							paste_slot_of(input_buf[di]));
				}
				memmove(input_buf + input_pos,
					input_buf + old_pos,
					input_len - old_pos);
				input_len -= (old_pos - input_pos);
				input_buf[input_len] = '\0';
				recalc_input_height();
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
				recalc_input_height();
				update_dynamic_statusbar();
			}
			break;
		}
	}
}

/* --- welcome screen --- */

/* embedded ASCII logo */
static const char *logo_lines[] = {
	"▗▖ ▗▖ ▗▄▖ ▗▄▄▖  ▗▄▖",
	"▐▌▗▞▘▐▌ ▐▌▐▌ ▐▌▐▌ ▐▌",
	"▐▛▚▖ ▐▌ ▐▌▐▛▀▚▖▐▛▀▜▌",
	"▐▌ ▐▌▝▚▄▞▘▐▌ ▐▌▐▌ ▐▌",
	NULL
};

static void draw_logo(void)
{
	wattron(win_chat, A_BOLD);
	for (const char **p = logo_lines; *p; p++)
		wprintw(win_chat, "  %s\n", *p);
	wattroff(win_chat, A_BOLD);
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
