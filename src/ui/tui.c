#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <locale.h>
#include <pthread.h>
#include <time.h>
#ifdef __APPLE__
#include <ncurses.h>
#else
#include <ncursesw/ncurses.h>
#endif

#include "db.h"
#include "event.h"
#include "markdown.h"
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
 *    CHAT  tab:next pane  enter:send  esc:cancel  status bar (1 line)
 */

/* ncurses windows */
static WINDOW *win_header = NULL;
static WINDOW *win_sessions = NULL;
static WINDOW *win_chat = NULL;
static WINDOW *win_admin = NULL;
static WINDOW *win_log = NULL;
static WINDOW *win_input = NULL;
static WINDOW *win_status = NULL;

/* right admin pane: width (pane is always visible; only collapsed as a
   last-resort safety when the remaining chat width would go negative) */
#define ADMIN_W_DEFAULT 28
static int admin_w = ADMIN_W_DEFAULT;
static int admin_visible = 1;

static struct tui_model_row *model_rows = NULL;
static int model_rows_n = 0;

/* alias of the model currently being pulled (empty = nothing downloading).
   written from background worker threads, read from the main thread inside
   draw_admin — guarded by a small mutex because the write is a strncpy and
   races could otherwise produce a partial-string frame. */
static pthread_mutex_t downloading_lock = PTHREAD_MUTEX_INITIALIZER;
static char downloading_alias[256] = "";

static int row_is_downloading(const char *alias)
{
	int yes;
	pthread_mutex_lock(&downloading_lock);
	yes = downloading_alias[0] != '\0' &&
	      strcmp(downloading_alias, alias) == 0;
	pthread_mutex_unlock(&downloading_lock);
	return yes;
}

/* synthetic row: displayed at the top of the MODELS pane when the current
   download target is NOT a registry alias (i.e. a URL pull). gives the user
   a visible, selectable row so they can press 'x' to cancel it — same gesture
   as cancelling an alias pull. recomputed on every draw_admin call. */
static struct {
	int  active;         /* 1 when we should render this row */
	char alias[256];     /* canonical target (URL or free-form name) — matches
	                        downloading_alias byte-for-byte so row_is_downloading
	                        fires and 'x' triggers /model_cancel. */
	char display[40];    /* short, human-friendly label for rendering */
} synth_row;

static int synth_row_active(void) { return synth_row.active; }
static int admin_effective_n(void) { return model_rows_n + (synth_row_active() ? 1 : 0); }

/* pick the display label for a URL-pull: basename of the path minus any
   query string, clamped to the display buffer. falls back to the raw alias
   if no '/' is present. */
static void derive_synth_display(const char *alias, char *out, size_t out_sz)
{
	const char *slash = strrchr(alias, '/');
	const char *base  = slash ? slash + 1 : alias;
	size_t blen = strlen(base);
	if (blen >= out_sz) blen = out_sz - 1;
	memcpy(out, base, blen);
	out[blen] = '\0';
	char *q = strchr(out, '?');
	if (q) *q = '\0';
	if (out[0] == '\0') {
		blen = strlen(alias);
		if (blen >= out_sz) blen = out_sz - 1;
		memcpy(out, alias, blen);
		out[blen] = '\0';
	}
}

static void refresh_synth_row(void)
{
	char target[256];
	pthread_mutex_lock(&downloading_lock);
	snprintf(target, sizeof target, "%s", downloading_alias);
	pthread_mutex_unlock(&downloading_lock);

	if (target[0] == '\0') { synth_row.active = 0; return; }

	for (int i = 0; i < model_rows_n; i++) {
		if (strcmp(model_rows[i].alias, target) == 0) {
			synth_row.active = 0;
			return;
		}
	}

	synth_row.active = 1;
	snprintf(synth_row.alias, sizeof synth_row.alias, "%s", target);
	derive_synth_display(target, synth_row.display, sizeof synth_row.display);
}

/* --- kai color palette (matches kc) ---
 * ncurses init_color() takes 0–1000 scale; hex→1000: n*1000/255
 */
#define KAI_BLACK       16  /* #161616 */
#define KAI_RED         17  /* #e85555 */
#define KAI_GREEN       18  /* #6ab050 */
#define KAI_ORANGE      19  /* #e38735 */
#define KAI_BLUE        20  /* #5c8dcc */
#define KAI_MAGENTA     21  /* #9664dc */
#define KAI_CYAN        22  /* #50beb4 */
#define KAI_WHITE       23  /* #c8c8c8 */
#define KAI_DIM         24  /* #555555 */
#define KAI_BRIGHTWHITE 25  /* #eeeeee */
#define KAI_HIGHLIGHT   26  /* #2a2a2a */
#define KAI_BORDER      27  /* #333333 */
#define KAI_PROSE_BG    28  /* #1a1a1a — subtle assistant-message tint */

/* color pairs */
#define CP_FG            1
#define CP_ACCENT        2
#define CP_DIM           3
#define CP_BORDER        4
#define CP_SELECTED      5   /* black on orange — active session row */
#define CP_USER          6   /* orange bold — user messages */
#define CP_ASSISTANT     7   /* white — assistant messages */
#define CP_DAEMON_UP     8   /* green — daemon reachable */
#define CP_DAEMON_DOWN   9   /* red   — daemon unreachable */
/* markdown styling — block / inline decorations for assistant post-render.
   kept in a contiguous range so the future syntax-highlighting palette can
   start at CP_SYNTAX_BASE without colliding. */
#define CP_MD_HEADING    10  /* magenta — h1/h2/h3 (bg = prose tint) */
#define CP_MD_CODE_INLINE 11 /* cyan — inline `code` (bg = prose tint) */
#define CP_MD_CODE_BLOCK 12  /* cyan dim — fenced code blocks (darker bg) */
#define CP_MD_QUOTE      13  /* dim — blockquote text (bg = prose tint) */
#define CP_MD_PROSE      14  /* white on prose tint — baseline assistant text */

static int colors_available = 0;
/* deprecated alias kept so the render paths compile while we finish theming */
#define CP_USER_MSG CP_USER

/* queued user message — set while a generation / compaction is in flight so
   the next message is sent automatically on completion. rendered as a tail
   appended to the bottom statusbar hint (see draw_statusbar). */
static char queued_preview[160] = "";

/* operational log: ring buffer of events shown in the LOG pane under MODELS.
   anything a background thread wants to report (download progress, model
   switch, compaction, etc.) goes here instead of the chat pad. */
#define LOG_RING_SIZE 512
struct log_entry { time_t ts; char *text; };
static struct log_entry log_ring[LOG_RING_SIZE];
static int log_head  = 0;   /* next write index */
static int log_count = 0;   /* valid entries, capped at LOG_RING_SIZE */
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int log_dirty = 0;  /* main-thread redraw request */

/* rule 2 (horizontal separator above the input box) state. published by
   layout_windows so draw_rule2 below can redraw on demand. */
static int g_rule2_y              = -1;
static int g_rule2_chat_start     = 0;
static int g_rule2_full_end       = 0;
static int g_rule2_ltee_col       = 0;
static int g_rule2_rtee_col       = -1;

static void draw_rule2(void)
{
	if (g_rule2_y < 0) return;
	int border_cp = colors_available ? CP_BORDER : 0;

	if (border_cp) attron(COLOR_PAIR(border_cp));
	else           attron(A_DIM);
	for (int c = g_rule2_chat_start; c < g_rule2_full_end; c++)
		mvaddch(g_rule2_y, c, ACS_HLINE);
	mvaddch(g_rule2_y, g_rule2_ltee_col, ACS_LTEE);
	if (g_rule2_rtee_col >= 0)
		mvaddch(g_rule2_y, g_rule2_rtee_col, ACS_RTEE);
	if (border_cp) attroff(COLOR_PAIR(border_cp));
	else           attroff(A_DIM);
}

/* session pane (left) */
#define SESSIONS_W_DEFAULT 24
#define SESSIONS_W_MIN     20
static int sessions_w = SESSIONS_W_DEFAULT;
static int sessions_visible = 1;

struct tui_session_row {
	int  id;
	int  msg_count;
	char name[128];
};
static struct tui_session_row *sessions_rows = NULL;
static int sessions_rows_n = 0;
static int sessions_active_id = -1;

/* state */
static char header_mode[64] = "kora";
static char header_model[128] = "";
static char *statusbar_text = NULL;
static char *statusbar_right_text = NULL;
static int initialized = 0;
static int user_msg_attr = A_BOLD;  /* fallback if no colors */
static int daemon_status = -1;      /* -1 unknown, 0 down, 1 up */
static char header_session[128] = "";

/* pane-focus navigation. enum values define the Tab cycle order;
   values are deliberately left-to-right so Tab reads "natural". */
enum tui_focus { FOCUS_SESSIONS = 0, FOCUS_CHAT = 1, FOCUS_ADMIN = 2 };
static int tui_focus = FOCUS_CHAT;        /* default: ready to type */
static int sessions_hl_idx     = 0;  /* highlight index in session list */
static int sessions_scroll_top = 0;  /* first visible row */
static int models_hl_idx       = 0;  /* highlight index in model list (effective) */
static int models_scroll_top   = 0;  /* first visible row (effective idx) */

/* helper: clamp a highlight index to [0, count-1]; returns 0 if count<=0 */
static int clamp_hl(int hl, int count)
{
	if (count <= 0) return 0;
	if (hl < 0) return 0;
	if (hl >= count) return count - 1;
	return hl;
}

/* set asynchronously by SIGWINCH; main loop rebuilds the layout when true */
static volatile sig_atomic_t resize_pending = 0;

/* set by background threads (e.g. daemon poller) to push a new status;
   the main loop applies it on the next tick so ncurses stays single-threaded.
   -1 = no pending update. */
static volatile int pending_daemon_status = -1;

/* set by background ops (generation, compaction) to let Esc cancel them.
   0 = no cancellable op, 1 = one is running. main.c toggles via
   tui_set_cancellable(). */
static volatile int cancellable_active = 0;

/* set by bg_pull_fn when a download finishes — main thread refreshes the
   MODELS pane from DB state. callback registered by main.c. */
static volatile int pending_models_refresh = 0;
static void (*models_refresh_cb)(void) = NULL;

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
#define INPUT_H_MIN 1         /* single-row input by default; grows with \n */
#define INPUT_H_MAX 10
#define STATUS_H   1
#define BORDER_H   1
/* breathing room for chat pad; keeps text off both dividers. */
#define CHAT_PAD_LEFT  2
#define CHAT_PAD_RIGHT 2

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

/* chat log: source-of-truth buffer for everything that's been rendered into
   the chat pad. needed because ncurses pads don't reflow on resize — we have
   to wipe and re-render from a known-good source. every render_* path that
   writes to the pad also appends here. */
enum chat_log_kind {
	CLK_USER,
	CLK_ASSISTANT,
	CLK_INFO,
};
struct chat_log_entry {
	enum chat_log_kind kind;
	char *text;
};
static struct chat_log_entry *chat_log = NULL;
static int chat_log_n = 0;
static int chat_log_cap = 0;

/* streaming assistant buffer. chunks accumulate here until tui_assistant_end
   flushes the full text into the log as a single CLK_ASSISTANT entry. on
   resize-mid-stream we replay the log, then replay this partial on top. */
static char *assistant_pending = NULL;
static size_t assistant_pending_len = 0;
static size_t assistant_pending_cap = 0;
static int assistant_pending_active = 0;

/* poll interval for non-blocking input (ms) */
#define POLL_INTERVAL_MS 16

/* forward declarations: defined later but used in draw_statusbar */
static const char *focus_label(void);
static const char *default_statusbar_for_focus(void);

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
	if (c == 1) return 0;   /* \x01: hidden command-mode marker */
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

	/* chat_x is where the chat pad text begins on screen — past the
	   divider and a small left margin so text doesn't touch the line. */
	int chat_x = sessions_w + 1 + CHAT_PAD_LEFT;
	pnoutrefresh(win_chat, top, 0, chat_y, chat_x,
	             chat_y + chat_h - 1, chat_x + chat_cols - 1);
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

	/* focus indicator (left anchor) — colored, bold. matches kc's accent usage. */
	const char *label = focus_label();
	int label_cp = (tui_focus == FOCUS_CHAT) ? CP_ACCENT : CP_FG;
	wattron(win_status, A_BOLD | COLOR_PAIR(label_cp));
	mvwprintw(win_status, 0, 1, "%s", label);
	wattroff(win_status, A_BOLD | COLOR_PAIR(label_cp));

	int left_end = 1 + (int)strlen(label);
	int right_anchor = w;

	/* right anchor: ctx / pct (dim) */
	if (statusbar_right_text) {
		int rlen = (int)strlen(statusbar_right_text);
		int rx = w - rlen - 1;
		if (rx > left_end + 2) {
			wattron(win_status, COLOR_PAIR(CP_DIM));
			mvwprintw(win_status, 0, rx, "%s", statusbar_right_text);
			wattroff(win_status, COLOR_PAIR(CP_DIM));
			right_anchor = rx;
		}
	}

	/* contextual hints between mode and ctx (dim).
	   statusbar_text NULL = show the focus-default; non-NULL = transient
	   message (e.g. "generating..."). recomputing the default on every
	   draw means Tab-switching picks up the new pane's hint for free.
	   if a pending message queue is set, append it to whatever hint is
	   active — composing in-place keeps main.c from having to juggle
	   statusbar strings when a new turn starts while others are queued. */
	const char *hint = statusbar_text
	                 ? statusbar_text
	                 : default_statusbar_for_focus();
	char composed[320];
	if (queued_preview[0]) {
		if (hint && *hint)
			snprintf(composed, sizeof composed, "%s · %s", hint, queued_preview);
		else
			snprintf(composed, sizeof composed, "%s", queued_preview);
		hint = composed;
	}
	if (hint && *hint) {
		wattron(win_status, COLOR_PAIR(CP_DIM));
		int hint_start = left_end + 3;   /* mode + 2-col gutter + 1 */
		int hint_max   = right_anchor - hint_start - 1;
		if (hint_max > 0) {
			int hlen = (int)strlen(hint);
			if (hlen > hint_max) hlen = hint_max;
			mvwprintw(win_status, 0, hint_start, "%.*s", hlen, hint);
		}
		wattroff(win_status, COLOR_PAIR(CP_DIM));
	}

	wnoutrefresh(win_status);
}

/* truncate `src` to fit in `max` visible columns. writes into `out` (size
   out_sz) adding an ellipsis char when truncation was needed. returns the
   printable width used. */
static int truncate_name(const char *src, int max, char *out, int out_sz)
{
	if (max <= 0 || out_sz <= 1) {
		if (out_sz > 0) out[0] = '\0';
		return 0;
	}
	int len = (int)strlen(src);
	if (len <= max) {
		snprintf(out, (size_t)out_sz, "%s", src);
		return len;
	}
	if (max >= 2) {
		int cut = max - 1;
		if (cut >= out_sz - 4) cut = out_sz - 4;
		snprintf(out, (size_t)out_sz, "%.*s…", cut, src);
		return cut + 1;
	}
	snprintf(out, (size_t)out_sz, "%.*s", max, src);
	return max;
}

static void draw_log(void)
{
	if (!win_log) return;
	int w = getmaxx(win_log);
	int h = getmaxy(win_log);
	if (w < 6 || h < 2) { wnoutrefresh(win_log); return; }

	werase(win_log);

	/* title */
	wattron(win_log, A_BOLD | COLOR_PAIR(CP_ACCENT));
	mvwprintw(win_log, 0, 1, "Activity");
	wattroff(win_log, A_BOLD | COLOR_PAIR(CP_ACCENT));

	pthread_mutex_lock(&log_lock);
	int width = w - 2;
	if (width < 1) width = 1;

	/* reverse-chronological with soft-wrap: newest entry at the top,
	   long entries span multiple rows. timestamp in accent color,
	   message in dim color, continuation lines start at column 1. */
	int newest = (log_head - 1 + LOG_RING_SIZE) % LOG_RING_SIZE;
	int row = 2;

	for (int i = 0; i < log_count && row < h; i++) {
		int idx = (newest - i + LOG_RING_SIZE) % LOG_RING_SIZE;
		struct log_entry *e = &log_ring[idx];
		char line[256];
		struct tm lt;
		localtime_r(&e->ts, &lt);

		/* first line: timestamp in accent color, then message */
		int ts_len = snprintf(line, sizeof line, "%02d:%02d ", lt.tm_hour, lt.tm_min);
		const char *text = e->text ? e->text : "";
		int msg_len = (int)strlen(text);

		/* render timestamp in accent color */
		wattron(win_log, COLOR_PAIR(CP_ACCENT) | A_DIM);
		mvwaddnstr(win_log, row, 1, line, ts_len);
		wattroff(win_log, COLOR_PAIR(CP_ACCENT) | A_DIM);

		/* wrap message: no indent on continuation lines */
		int off = 0;
		int first_avail = width - ts_len;
		if (first_avail < 1) first_avail = 1;
		int chunk = (msg_len > first_avail) ? first_avail : msg_len;
		wattron(win_log, COLOR_PAIR(CP_DIM));
		mvwaddnstr(win_log, row, 1 + ts_len, text, chunk);
		wattroff(win_log, COLOR_PAIR(CP_DIM));
		row++;
		off += chunk;

		/* continuation lines: no indent, dimmer color */
		while (off < msg_len && row < h) {
			int avail = width;
			if (avail < 1) avail = 1;
			chunk = (msg_len - off > avail) ? avail : (msg_len - off);
			wattron(win_log, COLOR_PAIR(CP_DIM));
			mvwaddnstr(win_log, row, 1, text + off, chunk);
			wattroff(win_log, COLOR_PAIR(CP_DIM));
			row++;
			off += chunk;
		}
	}
	pthread_mutex_unlock(&log_lock);

	wnoutrefresh(win_log);
}

void tui_log(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);

	pthread_mutex_lock(&log_lock);
	if (log_count == LOG_RING_SIZE) {
		/* ring is full: the slot at log_head is the oldest entry that
		   we're about to overwrite. free its text before reuse. */
		free(log_ring[log_head].text);
	} else {
		log_count++;
	}
	log_ring[log_head].ts   = time(NULL);
	log_ring[log_head].text = strdup(buf);
	log_head = (log_head + 1) % LOG_RING_SIZE;
	pthread_mutex_unlock(&log_lock);

	/* thread-safe: request a main-thread redraw. the poll loop inside
	   tui_input services this on its next tick. */
	log_dirty = 1;
}

static void draw_admin(void)
{
	if (!win_admin) return;
	int w = getmaxx(win_admin);
	int h = getmaxy(win_admin);
	if (w < 6 || h < 3) return;

	werase(win_admin);

	/* title */
	wattron(win_admin, A_BOLD | COLOR_PAIR(CP_ACCENT));
	mvwprintw(win_admin, 0, 1, "Models");
	wattroff(win_admin, A_BOLD | COLOR_PAIR(CP_ACCENT));

	int row = 2;
	int inner = w - 2;
	(void)inner;

	refresh_synth_row();
	int eff_n = admin_effective_n();

	if (eff_n == 0) {
		wattron(win_admin, COLOR_PAIR(CP_DIM));
		mvwprintw(win_admin, 2, 2, "(registry empty)");
		wattroff(win_admin, COLOR_PAIR(CP_DIM));
		wnoutrefresh(win_admin);
		return;
	}

	/* clamp highlight + keep it in the visible window. the render loop
	   below fills rows 2..h-1 inclusive, giving capacity h-2 entries
	   (title + blank row at the top). */
	models_hl_idx = clamp_hl(models_hl_idx, eff_n);
	int visible = h - 2;
	if (visible < 1) visible = 1;
	if (models_hl_idx < models_scroll_top)
		models_scroll_top = models_hl_idx;
	if (models_hl_idx >= models_scroll_top + visible)
		models_scroll_top = models_hl_idx - visible + 1;
	if (models_scroll_top > eff_n - visible)
		models_scroll_top = eff_n - visible;
	if (models_scroll_top < 0) models_scroll_top = 0;

	int synth_offset = synth_row_active() ? 1 : 0;

	/* render the synthetic (URL-pull) row at effective index 0 — but
	   only when scroll hasn't carried us past it. */
	if (synth_row_active() && models_scroll_top == 0 && row < h) {
		int is_highlighted = (tui_focus == FOCUS_ADMIN && 0 == models_hl_idx);
		int name_max = w - 4;
		if (name_max < 1) name_max = 1;
		char aname[64];
		truncate_name(synth_row.display, name_max, aname, sizeof aname);

		if (is_highlighted) {
			wattron(win_admin, COLOR_PAIR(CP_SELECTED));
			mvwhline(win_admin, row, 0, ' ', w);
			mvwprintw(win_admin, row, 1, "%s %s", "⟳", aname);
			wattroff(win_admin, COLOR_PAIR(CP_SELECTED));
		} else {
			wattron(win_admin, COLOR_PAIR(CP_DIM));
			mvwprintw(win_admin, row, 1, "%s %s", "⟳", aname);
			wattroff(win_admin, COLOR_PAIR(CP_DIM));
		}
		row++;
	}

	/* walk the registry rows, skipping those above the scroll window. */
	int start_real = models_scroll_top - synth_offset;
	if (start_real < 0) start_real = 0;
	for (int i = start_real; i < model_rows_n && row < h; i++, row++) {
		int eff_idx        = i + synth_offset;
		int is_current     = model_rows[i].is_current;
		int is_highlighted = (tui_focus == FOCUS_ADMIN && eff_idx == models_hl_idx);
		int is_downloaded  = model_rows[i].downloaded;
		int is_downloading = row_is_downloading(model_rows[i].alias);

		const char *bullet = is_downloading ? "⟳"
		                   : is_current     ? "●"
		                   : is_downloaded  ? "○"
		                                    : "·";

		const char *sz = model_rows[i].size[0] ? model_rows[i].size : "";
		int sz_w = (int)strlen(sz);

		int name_max = w - 4 - (sz_w > 0 ? sz_w + 1 : 0);
		if (name_max < 1) name_max = 1;
		char aname[96];
		const char *label = model_rows[i].display[0]
		                  ? model_rows[i].display
		                  : model_rows[i].alias;
		truncate_name(label, name_max, aname, sizeof aname);

		/* downloading doesn't override the row's base color — the ⟳
		   glyph is enough. keeps the pane from looking like two active
		   (accent/bold) rows at once during a pull. */
		int cp = is_current    ? CP_ACCENT
		       : is_downloaded ? CP_FG
		                       : CP_DIM;
		int bold = is_current;

		if (is_highlighted) {
			wattron(win_admin, COLOR_PAIR(CP_SELECTED));
			mvwhline(win_admin, row, 0, ' ', w);
			mvwprintw(win_admin, row, 1, "%s %s", bullet, aname);
			if (sz_w > 0)
				mvwprintw(win_admin, row, w - sz_w - 1, "%s", sz);
			wattroff(win_admin, COLOR_PAIR(CP_SELECTED));
		} else {
			wattron(win_admin, COLOR_PAIR(cp));
			if (bold) wattron(win_admin, A_BOLD);
			mvwprintw(win_admin, row, 1, "%s %s", bullet, aname);
			if (bold) wattroff(win_admin, A_BOLD);
			wattroff(win_admin, COLOR_PAIR(cp));
			if (sz_w > 0) {
				wattron(win_admin, COLOR_PAIR(CP_DIM));
				mvwprintw(win_admin, row, w - sz_w - 1, "%s", sz);
				wattroff(win_admin, COLOR_PAIR(CP_DIM));
			}
		}
	}

	wnoutrefresh(win_admin);
}

static void draw_sessions(void)
{
	if (!win_sessions) return;
	int w = getmaxx(win_sessions);
	int h = getmaxy(win_sessions);
	if (w < 4 || h < 3) return;

	werase(win_sessions);

	/* title (no under-rule; kc-style clean spacing) */
	wattron(win_sessions, A_BOLD | COLOR_PAIR(CP_ACCENT));
	mvwprintw(win_sessions, 0, 1, "Sessions");
	wattroff(win_sessions, A_BOLD | COLOR_PAIR(CP_ACCENT));

	int visible_rows = h - 2;  /* title row + blank row at the top */

	/* clamp highlight + keep it in the visible window */
	sessions_hl_idx = clamp_hl(sessions_hl_idx, sessions_rows_n);
	if (visible_rows <= 0) visible_rows = 1;
	if (sessions_hl_idx < sessions_scroll_top)
		sessions_scroll_top = sessions_hl_idx;
	if (sessions_hl_idx >= sessions_scroll_top + visible_rows)
		sessions_scroll_top = sessions_hl_idx - visible_rows + 1;
	if (sessions_scroll_top > sessions_rows_n - visible_rows)
		sessions_scroll_top = sessions_rows_n - visible_rows;
	if (sessions_scroll_top < 0) sessions_scroll_top = 0;

	int shown = 0;
	for (int i = sessions_scroll_top;
	     i < sessions_rows_n && shown < visible_rows;
	     i++, shown++) {
		int row = 2 + shown;
		int is_active      = (sessions_rows[i].id == sessions_active_id);
		int is_highlighted = (tui_focus == FOCUS_SESSIONS && i == sessions_hl_idx);

		const char *name = sessions_rows[i].name[0] ? sessions_rows[i].name : "New session";

		/* right-side message count: "  12" (or empty if 0) */
		char count_str[12] = "";
		int count_w = 0;
		if (sessions_rows[i].msg_count > 0) {
			snprintf(count_str, sizeof count_str, "%d", sessions_rows[i].msg_count);
			count_w = (int)strlen(count_str);
		}

/* layout:  " name…     12"
		 *          ^1          ^count
		 * inner width = w - 1 (left margin) */
		int inner    = w - 1;
		int name_max = inner - count_w - (count_w > 0 ? 5 : 0);
		if (name_max < 1) name_max = 1;

		char name_buf[128];
		truncate_name(name, name_max, name_buf, sizeof name_buf);

		/* three visual states:
		   - highlighted (pane focused, this row is the cursor): reverse bar
		   - active (this session is the currently-open one): accent color
		   - neither: plain fg, dim count */
		if (is_highlighted) {
			wattron(win_sessions, COLOR_PAIR(CP_SELECTED));
			mvwhline(win_sessions, row, 0, ' ', w);
			mvwprintw(win_sessions, row, 1, "%s%s",
			          is_active ? "● " : "  ", name_buf);
			if (count_w > 0)
				mvwprintw(win_sessions, row, w - count_w - 1, "%s", count_str);
			wattroff(win_sessions, COLOR_PAIR(CP_SELECTED));
		} else if (is_active) {
			wattron(win_sessions, A_BOLD | COLOR_PAIR(CP_ACCENT));
			mvwprintw(win_sessions, row, 1, "● %s", name_buf);
			wattroff(win_sessions, A_BOLD | COLOR_PAIR(CP_ACCENT));
			if (count_w > 0) {
				wattron(win_sessions, COLOR_PAIR(CP_DIM));
				mvwprintw(win_sessions, row, w - count_w - 1, "%s", count_str);
				wattroff(win_sessions, COLOR_PAIR(CP_DIM));
			}
		} else {
			wattron(win_sessions, COLOR_PAIR(CP_FG));
			mvwprintw(win_sessions, row, 1, "  %s", name_buf);
			wattroff(win_sessions, COLOR_PAIR(CP_FG));
			if (count_w > 0) {
				wattron(win_sessions, COLOR_PAIR(CP_DIM));
				mvwprintw(win_sessions, row, w - count_w - 1, "%s", count_str);
				wattroff(win_sessions, COLOR_PAIR(CP_DIM));
			}
		}
	}

	if (sessions_rows_n == 0) {
		wattron(win_sessions, COLOR_PAIR(CP_DIM));
		mvwprintw(win_sessions, 2, 2, "(no sessions yet)");
		wattroff(win_sessions, COLOR_PAIR(CP_DIM));
	}

	wnoutrefresh(win_sessions);
}

static void draw_header(void)
{
	if (!win_header) return;

	int cols = getmaxx(win_header);
	int mid = HEADER_H / 2;

	werase(win_header);

	/* left: app/mode name */
	wattron(win_header, A_BOLD | COLOR_PAIR(CP_ACCENT));
	mvwprintw(win_header, mid, 2, "%s", header_mode);
	wattroff(win_header, A_BOLD | COLOR_PAIR(CP_ACCENT));

	int left_end = 2 + (int)strlen(header_mode);

	/* center-left: session name (separator + name) if set */
	if (header_session[0]) {
		wattron(win_header, COLOR_PAIR(CP_DIM));
		mvwprintw(win_header, mid, left_end + 1, "·");
		wattroff(win_header, COLOR_PAIR(CP_DIM));
		wattron(win_header, COLOR_PAIR(CP_FG));
		mvwprintw(win_header, mid, left_end + 3, "%s", header_session);
		wattroff(win_header, COLOR_PAIR(CP_FG));
	}

	/* build the right side: "model  ●"  (dot colored by daemon status) */
	int right_anchor = cols - 2;  /* 2-col right margin */

	/* daemon dot */
	int dot_cp = (daemon_status == 1) ? CP_DAEMON_UP
	           : (daemon_status == 0) ? CP_DAEMON_DOWN
	           : CP_DIM;
	const char *dot = (daemon_status == 1) ? "●"
	                : (daemon_status == 0) ? "○"
	                : "◌";
	wattron(win_header, COLOR_PAIR(dot_cp));
	mvwprintw(win_header, mid, right_anchor - 1, "%s", dot);
	wattroff(win_header, COLOR_PAIR(dot_cp));

	if (header_model[0]) {
		int model_len = (int)strlen(header_model);
		wattron(win_header, COLOR_PAIR(CP_DIM));
		mvwprintw(win_header, mid, right_anchor - 1 - model_len - 2,
		          "%s", header_model);
		wattroff(win_header, COLOR_PAIR(CP_DIM));
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

	vis_lines = input_h; /* no borders — full height is content */
	w = input_content_width();
	if (w < 1) w = 1;

	input_pos_to_visual(input_pos, &cur_vrow, &cur_vcol);

	/* adjust scroll to keep cursor visible */
	if (cur_vrow < input_scroll)
		input_scroll = cur_vrow;
	if (cur_vrow >= input_scroll + vis_lines)
		input_scroll = cur_vrow - vis_lines + 1;

	werase(win_input);

	/* no box() — kc-style minimal; focus label lives in the status bar.
	   when the buffer starts with \x01 (SOH) we're in command-entry
	   (user pressed r/p/x from SESSIONS/ADMIN and was bumped to CHAT to
	   type an argument): prompt becomes ":" and the hidden marker is
	   skipped during rendering. */
	int is_cmd = (input_len > 0 && (unsigned char)input_buf[0] == 1);
	vl = 0;
	col = 0;
	needs_prefix = 1;

	for (i = 0; i <= input_len; i++) {
		if (vl >= input_scroll + vis_lines)
			break;

		/* draw prefix at start of each visible visual line */
		if (needs_prefix && vl >= input_scroll) {
			int row = vl - input_scroll;
			if (vl == 0) {
				wattron(win_input, A_BOLD | COLOR_PAIR(CP_ACCENT));
				mvwprintw(win_input, row, 1, is_cmd ? ": " : "> ");
				wattroff(win_input, A_BOLD | COLOR_PAIR(CP_ACCENT));
			} else {
				mvwprintw(win_input, row, 1, "  ");
			}
			needs_prefix = 0;
		}

		if (i == input_len)
			break;

		/* hidden command marker: skip entirely, don't advance col */
		if ((unsigned char)input_buf[i] == 1) continue;

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
						int row = vl - input_scroll;
						mvwprintw(win_input, row, 1, "  ");
						needs_prefix = 0;
					}
				}
				if (vl >= input_scroll) {
					int row = vl - input_scroll;
					wattron(win_input, A_DIM);
					mvwaddch(win_input, row, 3 + col, badge[j]);
					wattroff(win_input, A_DIM);
				}
				col++;
			}
			continue;
		}

		/* draw regular character if on a visible visual line */
		if (vl >= input_scroll) {
			int row = vl - input_scroll;
			mvwaddch(win_input, row, 3 + col, input_buf[i]);
		}

		col++;
		if (col >= w) {
			vl++;
			col = 0;
			needs_prefix = 1;
		}
	}

	/* position cursor */
	wmove(win_input, cur_vrow - input_scroll, 3 + cur_vcol);
	wrefresh(win_input);
}

static void layout_windows(void)
{
	int rows, cols;
	int input_y;

	getmaxyx(stdscr, rows, cols);

	/* dynamic sessions pane width: shrink on narrow terminals */
	sessions_w = sessions_visible ? SESSIONS_W_DEFAULT : 0;
	if (sessions_visible) {
		if (cols < 80) sessions_w = SESSIONS_W_MIN;
		if (cols < sessions_w + 40) sessions_w = cols / 4;
		if (sessions_w < SESSIONS_W_MIN) sessions_w = SESSIONS_W_MIN;
	}

	/* layout:
	 *   header(3)
	 *   ── rule 1 (full width, below header)
	 *   body: sessions pane │ chat area
	 *                                  where chat area =
	 *                                    chat_pad
	 *                                    ── rule 2 (chat-side only)
	 *                                    input
	 *   ── rule 3 (full width, above status)
	 *   status
	 *
	 * Sessions pane spans the ENTIRE body height. Rule 2 (input's top
	 * edge) only lives in the chat column — it doesn't cut into the
	 * sessions pane. Rule 3 runs full-width as the bottom divider above
	 * the status bar. The vertical divider terminates at rule 3 with a
	 * ┴ glyph.
	 */
	int body_h = rows - HEADER_H - BORDER_H - STATUS_H;   /* rows - 5 */
	chat_h = body_h - BORDER_H - input_h - BORDER_H;      /* minus rule3 + input + rule2 */
	if (chat_h < 1) chat_h = 1;
	chat_y = HEADER_H + BORDER_H;

	/* admin pane: respect visibility toggle, shrink on narrow terminals */
	admin_w = admin_visible ? ADMIN_W_DEFAULT : 0;
	int show_admin = admin_visible;
	const int ADMIN_W_MIN   = 14;
	const int SESSIONS_W_HARD_MIN = 14;
	const int CHAT_COLS_MIN = 8;
	int fixed = 1 + CHAT_PAD_LEFT + CHAT_PAD_RIGHT + 1;  /* two dividers + pads */
	int avail = cols - fixed;                             /* shared by sessions+chat+admin */

	/* step 1: shrink admin if default won't leave enough chat */
	if (sessions_w + admin_w + CHAT_COLS_MIN > avail) {
		admin_w = avail - sessions_w - CHAT_COLS_MIN;
		if (admin_w < ADMIN_W_MIN) admin_w = ADMIN_W_MIN;
	}
	/* step 2: if admin at its min still won't fit, shrink sessions too */
	if (sessions_w + admin_w + CHAT_COLS_MIN > avail) {
		sessions_w = avail - admin_w - CHAT_COLS_MIN;
		if (sessions_w < SESSIONS_W_HARD_MIN) sessions_w = SESSIONS_W_HARD_MIN;
	}

	int admin_col      = cols - admin_w;
	int right_reserved = admin_w + 1;
	chat_cols = cols - sessions_w - 1 - CHAT_PAD_LEFT - CHAT_PAD_RIGHT
	            - right_reserved;
	if (chat_cols < 1) chat_cols = 1;
	int chat_x = sessions_w + 1 + CHAT_PAD_LEFT;

	int sessions_h = body_h - BORDER_H;          /* stops just above rule3 */
	int rule2_y    = chat_y + chat_h;            /* chat-side only */
	input_y        = rule2_y + BORDER_H;
	int rule3_y    = input_y + input_h;          /* full width */
	int status_y   = rule3_y + BORDER_H;

	/* kc-style: create windows once, then wresize + mvwin on every call.
	   reusing persistent windows avoids the flicker and race you get
	   from delwin/newwin on each resize tick. */

	if (!win_header) {
		win_header = newwin(HEADER_H, cols, 0, 0);
	} else {
		wresize(win_header, HEADER_H, cols);
		mvwin(win_header, 0, 0);
	}

	if (!sessions_visible || sessions_w <= 0) {
		if (win_sessions) { delwin(win_sessions); win_sessions = NULL; }
	} else if (!win_sessions) {
		win_sessions = newwin(sessions_h, sessions_w, chat_y, 0);
	} else {
		wresize(win_sessions, sessions_h, sessions_w);
		mvwin(win_sessions, chat_y, 0);
	}

	/* right pane: MODELS on top, LOG below, separated by a 1-row divider.
	   MODELS is content-sized (title + rows) up to half the pane height;
	   LOG takes the rest so newest events stay visible without scroll. */
	int admin_h = 0, log_y = 0, admin_div_y = 0, log_h = 0;
	if (show_admin) {
		int rows_needed = 2 + model_rows_n;
		if (synth_row_active()) rows_needed++;
		if (rows_needed < 5) rows_needed = 5;
		int admin_max = sessions_h - 5;  /* ensure ≥3 log rows + divider */
		if (admin_max < rows_needed) admin_max = rows_needed;
		admin_h = rows_needed;
		if (admin_h > sessions_h / 2 && sessions_h >= 10)
			admin_h = sessions_h / 2;
		if (admin_h > admin_max) admin_h = admin_max;
		if (admin_h < 3) admin_h = 3;

		admin_div_y = chat_y + admin_h;
		log_y       = admin_div_y + 1;
		log_h       = sessions_h - admin_h - 1;
		if (log_h < 1) { log_h = 0; admin_div_y = 0; log_y = 0; }

if (!admin_visible || admin_w <= 0) {
		if (win_admin) { delwin(win_admin); win_admin = NULL; }
	} else if (!win_admin) {
		win_admin = newwin(admin_h, admin_w, chat_y, admin_col);
	} else {
		wresize(win_admin, admin_h, admin_w);
		mvwin(win_admin, chat_y, admin_col);
	}

		if (log_h > 0) {
			if (!win_log) {
				win_log = newwin(log_h, admin_w, log_y, admin_col);
			} else {
				wresize(win_log, log_h, admin_w);
				mvwin(win_log, log_y, admin_col);
			}
		} else if (win_log) {
			delwin(win_log);
			win_log = NULL;
		}
	} else {
		if (win_admin) { delwin(win_admin); win_admin = NULL; }
		if (win_log)   { delwin(win_log);   win_log   = NULL; }
	}

	/* chat pad: virtual buffer; pad height is large + fixed */
	if (win_chat) {
		wresize(win_chat, pad_h, chat_cols);
	} else {
		if (pad_h < chat_h) pad_h = PAD_INITIAL_H;
		win_chat = newpad(pad_h, chat_cols);
		scrollok(win_chat, TRUE);
	}

	if (!win_input) {
		win_input = newwin(input_h, chat_cols, input_y, chat_x);
		keypad(win_input, TRUE);
	} else {
		wresize(win_input, input_h, chat_cols);
		mvwin(win_input, input_y, chat_x);
	}

	if (!win_status) {
		win_status = newwin(STATUS_H, cols, status_y, 0);
	} else {
		wresize(win_status, STATUS_H, cols);
		mvwin(win_status, status_y, 0);
	}

	/* wipe stdscr so stale borders from the previous geometry don't
	   bleed through (on resize we keep windows but stdscr is bare). */
	erase();

	int border_cp = colors_available ? CP_BORDER : 0;
	if (border_cp) attron(COLOR_PAIR(border_cp));
	else          attron(A_DIM);

	/* rule 1: below header (full width) */
	move(HEADER_H, 0);
	hline(ACS_HLINE, cols);

	/* rule 3: above status bar (full width) */
	move(rule3_y, 0);
	hline(ACS_HLINE, cols);

	/* vertical divider: from chat_y to rule3_y - 1 (full body height) */
	if (sessions_visible && sessions_w > 0) {
		for (int r = chat_y; r < rule3_y; r++)
			mvaddch(r, sessions_w, ACS_VLINE);
	}

	/* second vertical divider between chat and admin pane */
	if (show_admin) {
		int admin_div = admin_col - 1;
		for (int r = chat_y; r < rule3_y; r++)
			mvaddch(r, admin_div, ACS_VLINE);
		mvaddch(HEADER_H, admin_div, ACS_TTEE);
		mvaddch(rule3_y,  admin_div, ACS_BTEE);

		/* horizontal divider inside the admin column separating MODELS
		   from LOG. spans from the chat/admin vertical divider to the
		   right edge; terminates with a left-tee on the vertical. */
		if (log_h > 0) {
			for (int c = admin_col; c < cols; c++)
				mvaddch(admin_div_y, c, ACS_HLINE);
			mvaddch(admin_div_y, admin_div, ACS_LTEE);
		}
	}

	/* T-junctions for the left (sessions) divider (header + status sides —
	   the rule2 LTEE/RTEE is drawn by draw_rule2() itself). */
	if (sessions_visible && sessions_w > 0) {
		mvaddch(HEADER_H, sessions_w, ACS_TTEE);
		mvaddch(rule3_y,  sessions_w, ACS_BTEE);
	}

	if (border_cp) attroff(COLOR_PAIR(border_cp));
	else          attroff(A_DIM);

	/* rule 2: above the input, with T-junctions where it meets the
	   vertical dividers. publish the coordinates so draw_rule2() can
	   redraw on its own later if anything ever needs to. */
	g_rule2_y          = rule2_y;
	g_rule2_chat_start = (sessions_visible && sessions_w > 0) ? sessions_w + 1 : 0;
	g_rule2_full_end   = cols;
	g_rule2_ltee_col   = (sessions_visible && sessions_w > 0) ? sessions_w : -1;
	g_rule2_rtee_col   = show_admin ? admin_col - 1 : -1;
	draw_rule2();

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

/* bottom-bar hints — swapped based on focused pane. */
static const char *default_statusbar_chat =
	"enter:send  alt+enter:nl  alt+j/k:scroll  alt+h:help  alt+s/m:panes  tab:pane  esc:cancel  ^c:quit";
static const char *default_statusbar_sessions =
	"j/k:up/down  enter:open  n:new  d:del  r:ren  tab:next";
static const char *default_statusbar_admin_rm =
	"j/k:up/down  enter:switch  p:pull  u:url  d:rm      tab:next";
static const char *default_statusbar_admin_cancel =
	"j/k:up/down  enter:switch  p:pull  u:url  d:cancel  tab:next";
static const char *default_statusbar_for_focus(void)
{
	switch (tui_focus) {
	case FOCUS_CHAT:     return default_statusbar_chat;
	case FOCUS_SESSIONS: return default_statusbar_sessions;
	case FOCUS_ADMIN: {
		/* 'x' is context-sensitive on the ADMIN pane: it cancels the
		   pull when the highlighted row is currently downloading, and
		   otherwise removes the model from disk. mirror that in the
		   hint so the shortcut reflects what the keypress will do. */
		const char *hl = tui_highlighted_model_alias();
		if (hl && row_is_downloading(hl))
			return default_statusbar_admin_cancel;
		return default_statusbar_admin_rm;
	}
	}
	return "";
}
#define default_statusbar (default_statusbar_for_focus())

static const char *focus_label(void)
{
	switch (tui_focus) {
	case FOCUS_CHAT:     return "CHAT";
	case FOCUS_SESSIONS: return "SESSIONS";
	case FOCUS_ADMIN:    return "MODELS";
	}
	return "?";
}

/* slash-command autocomplete is no longer relevant — commands are driven
   by NORMAL-mode shortcuts, not typed. the status bar always shows the
   mode-appropriate default hint, recomputed at draw time via draw_statusbar
   when statusbar_text is NULL. */
static void update_dynamic_statusbar(void)
{
	statusbar_set_left(NULL);
}

/* --- chat log (source of truth for resize replay) --- */

static void chat_log_push(enum chat_log_kind kind, const char *text)
{
	if (chat_log_n >= chat_log_cap) {
		int new_cap = chat_log_cap == 0 ? 64 : chat_log_cap * 2;
		struct chat_log_entry *tmp = realloc(chat_log,
		                                     new_cap * sizeof(*chat_log));
		if (!tmp) return;
		chat_log = tmp;
		chat_log_cap = new_cap;
	}
	chat_log[chat_log_n].kind = kind;
	chat_log[chat_log_n].text = text ? strdup(text) : strdup("");
	chat_log_n++;
}

static void chat_log_clear(void)
{
	int i;
	for (i = 0; i < chat_log_n; i++)
		free(chat_log[i].text);
	chat_log_n = 0;
}

static void assistant_pending_reset(void)
{
	assistant_pending_len = 0;
	assistant_pending_active = 0;
	if (assistant_pending)
		assistant_pending[0] = '\0';
}

static void assistant_pending_append(const char *text, int len)
{
	if (!text || len <= 0) return;
	size_t need = assistant_pending_len + (size_t)len + 1;
	if (need > assistant_pending_cap) {
		size_t new_cap = assistant_pending_cap ? assistant_pending_cap : 256;
		while (new_cap < need) new_cap *= 2;
		char *tmp = realloc(assistant_pending, new_cap);
		if (!tmp) return;
		assistant_pending = tmp;
		assistant_pending_cap = new_cap;
	}
	memcpy(assistant_pending + assistant_pending_len, text, len);
	assistant_pending_len += (size_t)len;
	assistant_pending[assistant_pending_len] = '\0';
}

/* forward decls — replay calls the render_ helpers defined below */
static void render_user_msg(const char *text);
static void render_info(const char *text);
static void render_assistant_begin(void);
static void render_assistant_chunk(const char *text, int len);
static void render_assistant_end(void);
static void render_assistant_styled(const char *text);

/* toggled by main.c via tui_set_markdown; when off, assistant replies are
   rendered as plain text (the pre-markdown behaviour). declared here so
   chat_log_replay and the event handlers can reach it before the markdown
   renderer is defined. */
static int markdown_enabled = 1;

static void chat_log_replay(void)
{
	int i;
	if (!win_chat) return;
	werase(win_chat);
	wmove(win_chat, 0, 0);

	for (i = 0; i < chat_log_n; i++) {
		switch (chat_log[i].kind) {
		case CLK_USER:
			render_user_msg(chat_log[i].text);
			break;
		case CLK_ASSISTANT:
			render_assistant_begin();
			if (markdown_enabled)
				render_assistant_styled(chat_log[i].text);
			else
				render_assistant_chunk(chat_log[i].text,
				                       (int)strlen(chat_log[i].text));
			render_assistant_end();
			break;
		case CLK_INFO:
			render_info(chat_log[i].text);
			break;
		}
	}

	/* if an assistant reply is mid-stream, re-emit the prefix and the
	   bytes received so far; the next chunk will continue from here. */
	if (assistant_pending_active) {
		render_assistant_begin();
		if (assistant_pending_len > 0)
			render_assistant_chunk(assistant_pending,
			                       (int)assistant_pending_len);
	}

	chat_scroll = 0;
	refresh_chat();
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
	   so the background highlight is continuous (no gaps). the padded
	   writes hit exactly `cols` characters, so ncurses auto-wraps the
	   cursor to column 0 of the row below — no trailing \n needed
	   (adding one would create a double-blank before the next
	   renderer's own leading \n). */
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
	refresh_chat();
}

static void render_assistant_begin(void)
{
	if (!win_chat) return;
	chat_pad_ensure();
	/* leading blank line to separate from the preceding message (user
	   turn, info line, whatever). matches render_user_msg and
	   render_info so every message gets uniform vertical breathing. */
	wprintw(win_chat, "\n  ");
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

/* --- markdown post-render for assistant messages ---
   On tui_assistant_end the full message is pushed to chat_log and a replay
   is triggered. Replay routes assistant entries through this path, which
   parses the markdown once and paints styled spans. Streaming (chunk-by-
   chunk) still uses render_assistant_chunk above — we can't parse partial
   markdown reliably, so the pad flashes raw text live and styles it when
   the stream closes. */

struct md_pad {
	int   at_line_start;   /* cursor at col 0 — need leading indent */
	int   in_quote;        /* inside blockquote — dim text */
	int   in_code;         /* inside fenced block — different styling */
	int   in_header_cell;  /* inside a <th> — bold cell content */
	int   list_depth;      /* for nested bullet indent */
	int   heading_attrs;   /* active ncurses attrs for the open heading */
	char  code_lang[32];
};

static int heading_attrs_for_level(int level)
{
	/* differentiate h1/h2/h3+ so a nested outline isn't a wall of bold. */
	int a = COLOR_PAIR(CP_MD_HEADING);
	if (level <= 1)      a |= A_BOLD | A_UNDERLINE;
	else if (level == 2) a |= A_BOLD;
	/* level >= 3: plain colour */
	return a;
}

/* Resolve inline attrs to a single ncurses attribute word. Color pairs
   are exclusive (ncurses holds only one pair per char), so we pick the
   right pair based on context and combine it with bold/underline bits. */
static int md_inline_attrs(int kmd_attrs, int in_quote)
{
	int pair;
	if (kmd_attrs & KMD_ATTR_CODE_INLINE) pair = CP_MD_CODE_INLINE;
	else if (in_quote)                     pair = CP_MD_QUOTE;
	else                                    pair = CP_MD_PROSE;

	int a = COLOR_PAIR(pair);
	if (kmd_attrs & KMD_ATTR_BOLD)   a |= A_BOLD;
	/* A_ITALIC isn't portable — A_UNDERLINE reads well on most terms */
	if (kmd_attrs & KMD_ATTR_ITALIC) a |= A_UNDERLINE;
	if (in_quote && !(kmd_attrs & KMD_ATTR_CODE_INLINE)) a |= A_DIM;
	return a;
}

static void md_indent(struct md_pad *p)
{
	if (p->at_line_start) {
		/* plain 2-space outdent — the prose bg starts at col 2, so
		   every assistant line has a narrow untinted left margin (same
		   shape as code blocks). matches render_assistant_begin which
		   also writes this gutter without attrs. */
		waddstr(win_chat, "  ");
		p->at_line_start = 0;
	}
}

/* Advance to the next row. Every assistant row has the shape:
     cols [0..1]   plain (untinted gutter, matches code-block layout)
     cols [2..cols-1]  prose bg

   If the cursor is already on a fresh auto-wrapped row (x < 2), we write
   the plain gutter first so "empty" rows (paragraph breaks, spacers) keep
   the same left margin as content rows instead of tinting all the way to
   col 0. Padding to exactly `cols` chars relies on ncurses auto-wrap to
   advance the cursor — same trick as code rows, no explicit '\n'. */
static void md_newline(struct md_pad *p)
{
	int y, x;
	getyx(win_chat, y, x);
	(void)y;
	int cols = getmaxx(win_chat);

	if (x < 2) {
		/* fresh row — lay down the untinted gutter before tinting the rest. */
		waddstr(win_chat, "  ");
		x = 2;
	}

	int pad = cols - x;
	if (pad > 0) {
		wattron(win_chat, COLOR_PAIR(CP_MD_PROSE));
		for (int i = 0; i < pad; i++) waddch(win_chat, ' ');
		wattroff(win_chat, COLOR_PAIR(CP_MD_PROSE));
	}
	p->at_line_start = 1;
}

/* Emit one line of a fenced code block padded out to the pane's right
   edge so the block reads as a uniform rectangle. The leading "  " of the
   assistant gutter is written plain; everything from the code-inset "  "
   onward is filled with CP_MD_CODE_BLOCK background, including trailing
   padding. We write exactly `cols` characters — ncurses' auto-wrap then
   advances the cursor to the next row. Adding an explicit '\n' on top of
   that produces a double-spaced block (was: the v1 bug). */
static void md_emit_code_row(struct md_pad *p, const char *text, int len)
{
	md_indent(p);
	int cols = getmaxx(win_chat);
	/* cursor is now at column 2 (post gutter). code inset + text + pad
	   must fill the remaining `cols - 2` columns. */
	int pad = cols - 2 /* gutter */ - 2 /* code inset */ - len;
	if (pad < 0) pad = 0;
	wattron(win_chat, COLOR_PAIR(CP_MD_CODE_BLOCK));
	if (len > 0)
		wprintw(win_chat, "  %.*s%*s", len, text, pad, "");
	else
		wprintw(win_chat, "  %*s", pad, "");
	wattroff(win_chat, COLOR_PAIR(CP_MD_CODE_BLOCK));
	/* auto-wrap moves the cursor to the next row — same pattern as
	   render_user_msg. if the line didn't fill cols (impossibly narrow
	   terminal), the next md_indent + at_line_start check handles it. */
	p->at_line_start = 1;
}

static void md_emit_text(struct md_pad *p, int attrs,
                         const char *text, size_t len)
{
	/* split on explicit '\n' first, then wrap each segment manually so
	   continuation lines keep the assistant's 2-space gutter. ncurses'
	   native auto-wrap would drop at col 0 of the next row and break the
	   left margin — visible as "strange padding" on long replies. */
	const char *start = text;
	size_t remaining = len;
	int a = md_inline_attrs(attrs, p->in_quote);

	while (remaining > 0) {
		const char *nl = memchr(start, '\n', remaining);
		size_t seg = nl ? (size_t)(nl - start) : remaining;

		const char *s = start;
		size_t r = seg;
		while (r > 0) {
			md_indent(p);
			int cur_y, cur_x;
			getyx(win_chat, cur_y, cur_x);
			(void)cur_y;
			int cols = getmaxx(win_chat);
			/* leave the rightmost column for md_newline's bg pad so
			   we never trigger auto-wrap mid-attr. */
			int avail = cols - cur_x - 1;
			if (avail <= 0) {
				md_newline(p);
				continue;
			}

			size_t write_n = r <= (size_t)avail ? r : (size_t)avail;

			/* soft word-wrap: if we'd cut mid-word and there's a
			   space earlier in the chunk, break on it instead. */
			if (write_n < r) {
				size_t j = write_n;
				while (j > 0 && s[j] != ' ') j--;
				if (j > 0) write_n = j + 1;  /* include the space */
			}

			wattron(win_chat, a);
			waddnstr(win_chat, s, (int)write_n);
			wattroff(win_chat, a);
			s += write_n;
			r -= write_n;
			if (r > 0) md_newline(p);
		}

		if (nl) {
			md_newline(p);
			start    = nl + 1;
			remaining -= seg + 1;
		} else {
			break;
		}
	}
}

static void md_cb(int kind, int attrs, int level,
                  const char *text, size_t len, void *user)
{
	struct md_pad *p = user;

	switch (kind) {
	case KMD_TEXT:
		md_emit_text(p, attrs, text, len);
		break;

	case KMD_HARD_BREAK:
		md_newline(p);
		break;

	case KMD_PARAGRAPH_BREAK:
		if (!p->at_line_start) md_newline(p);
		md_newline(p);   /* one blank line between blocks */
		break;

	case KMD_HEADING_BEGIN:
		if (!p->at_line_start) md_newline(p);
		md_indent(p);
		p->heading_attrs = heading_attrs_for_level(level);
		wattron(win_chat, p->heading_attrs);
		break;

	case KMD_HEADING_END:
		wattroff(win_chat, p->heading_attrs);
		p->heading_attrs = 0;
		md_newline(p);
		break;

	case KMD_CODE_BLOCK_BEGIN:
		if (!p->at_line_start) md_newline(p);
		p->in_code = 1;
		size_t n = len < sizeof p->code_lang - 1 ? len : sizeof p->code_lang - 1;
		if (n) memcpy(p->code_lang, text, n);
		p->code_lang[n] = '\0';
		/* leading blank row with the code-block background so the block
		   reads as a framed container rather than per-line ribbons. */
		md_emit_code_row(p, "", 0);
		break;

	case KMD_CODE_BLOCK_LINE:
		md_emit_code_row(p, text, (int)len);
		break;

	case KMD_CODE_BLOCK_END:
		/* matching trailing blank row so the bottom of the block is a
		   clean rectangle like the top. */
		md_emit_code_row(p, "", 0);
		p->in_code = 0;
		p->code_lang[0] = '\0';
		break;

	case KMD_LIST_ITEM_BEGIN: {
		if (!p->at_line_start) md_newline(p);
		md_indent(p);
		wattron(win_chat, COLOR_PAIR(CP_MD_PROSE));
		for (int i = 1; i < level; i++) waddstr(win_chat, "  ");
		waddstr(win_chat, "• ");
		wattroff(win_chat, COLOR_PAIR(CP_MD_PROSE));
		p->at_line_start = 0;
		p->list_depth = level;
		break;
	}

	case KMD_LIST_ITEM_END:
		if (!p->at_line_start) md_newline(p);
		break;

	case KMD_BLOCKQUOTE_BEGIN:
		if (!p->at_line_start) md_newline(p);
		p->in_quote = 1;
		break;

	case KMD_BLOCKQUOTE_END:
		p->in_quote = 0;
		break;

	case KMD_TABLE_BEGIN:
		if (!p->at_line_start) md_newline(p);
		break;

	case KMD_TABLE_END:
		/* trailing blank is handled by the next block's PARA event. */
		break;

	case KMD_TABLE_ROW_BEGIN:
		md_indent(p);
		wattron(win_chat, COLOR_PAIR(CP_MD_PROSE));
		waddstr(win_chat, "| ");
		wattroff(win_chat, COLOR_PAIR(CP_MD_PROSE));
		p->at_line_start = 0;
		break;

	case KMD_TABLE_ROW_END:
		md_newline(p);
		break;

	case KMD_TABLE_CELL_BEGIN:
		if (level > 0) {
			p->in_header_cell = 1;
			wattron(win_chat, A_BOLD);
		}
		break;

	case KMD_TABLE_CELL_END:
		if (p->in_header_cell) {
			wattroff(win_chat, A_BOLD);
			p->in_header_cell = 0;
		}
		wattron(win_chat, COLOR_PAIR(CP_MD_PROSE));
		waddstr(win_chat, " | ");
		wattroff(win_chat, COLOR_PAIR(CP_MD_PROSE));
		break;
	}
}

static void render_assistant_styled(const char *text)
{
	if (!win_chat || !text) return;
	chat_pad_ensure();

	struct md_pad p = { .at_line_start = 1 };
	/* the assistant message already has a leading "\n  " from
	   render_assistant_begin; we start at column 0 of that line with the
	   2-space indent pending. */
	p.at_line_start = 0;  /* begin already wrote "  " */

	int rc = kora_markdown_parse(text, strlen(text), md_cb, &p);
	if (rc != 0) {
		/* parse failed — fall back to plain rendering so the user never
		   loses the message content. */
		waddnstr(win_chat, text, (int)strlen(text));
	}
}

static void render_info(const char *text)
{
	if (!win_chat) return;
	chat_pad_ensure();
	chat_scroll_bottom();

	/* uniform leading blank so info lines are separated the same way
	   user / assistant messages are. callers that want a tight block
	   (e.g. /help) pass embedded newlines in one call, which we
	   re-indent below so every line keeps the same 2-space prefix. */
	wattron(win_chat, A_DIM);
	waddch(win_chat, '\n');
	const char *line = text;
	for (;;) {
		const char *nl = strchr(line, '\n');
		size_t len = nl ? (size_t)(nl - line) : strlen(line);
		wprintw(win_chat, "  %.*s\n", (int)len, line);
		if (!nl) break;
		line = nl + 1;
	}
	wattroff(win_chat, A_DIM);
	refresh_chat();
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
			chat_log_push(CLK_INFO, ev.data ? ev.data : "");
			render_info(ev.data ? ev.data : "");
			break;
		case TUI_EV_STATUSBAR_LEFT:
			/* ev.data NULL = "clear the transient and fall back to
			   the focus-default hint". draw_statusbar handles the
			   fallback itself at render time, so we simply forward
			   NULL to statusbar_set_left here. */
			statusbar_set_left(ev.data);
			break;
		case TUI_EV_CHAT_BEGIN:
			assistant_pending_reset();
			assistant_pending_active = 1;
			render_assistant_begin();
			break;
		case TUI_EV_CHAT_CHUNK:
			assistant_pending_append(ev.data ? ev.data : "", ev.len);
			render_assistant_chunk(ev.data ? ev.data : "", ev.len);
			break;
		case TUI_EV_CHAT_END:
			/* finalize streaming buffer into a log entry; the pending
			   buffer no longer needs to be replayed separately. */
			if (assistant_pending_active) {
				chat_log_push(CLK_ASSISTANT,
				              assistant_pending ? assistant_pending : "");
				assistant_pending_reset();
			}
			render_assistant_end();
			/* with markdown enabled, replay the whole log so the just-
			   pushed entry is re-rendered through the styled path. replay
			   is idempotent (already used on resize) — cost is one full
			   pad repaint per completed message. */
			if (markdown_enabled)
				chat_log_replay();
			break;
		case TUI_EV_USER_MSG:
			chat_log_push(CLK_USER, ev.data ? ev.data : "");
			render_user_msg(ev.data ? ev.data : "");
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

/* available text width per row (no box now; content starts at col 3). */
static int input_content_width(void)
{
	if (!win_input) return 40;
	/* cols - left_pad(1) - "> "(2) - right_pad(1) */
	int w = getmaxx(win_input) - 4;
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
	int new_h = nlines;     /* no borders — 1 row per visual line */
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
		draw_sessions();
		draw_admin();
		draw_log();
		refresh_chat();
		draw_statusbar();
		/* draw_input_box is called by the caller after this returns */
	}
}

/* --- resize handler --- */

/* signal-safe: just set a flag. the main loop services the resize.
   directly calling ncurses ops from a signal handler is unsafe. */
static void on_resize(int sig)
{
	(void)sig;
	resize_pending = 1;
}

/* called from the main loop when resize_pending is set.
   ncurses auto-updates LINES/COLS on SIGWINCH; we just need to wipe the
   stdscr content, rebuild our windows, and redraw everything. endwin()
   is for exiting ncurses and must NOT be called here. */
static void service_resize(void)
{
	resize_pending = 0;
	/* force a full redraw */
	endwin();
	refresh();
	clear();
	layout_windows();
	draw_header();
	draw_sessions();
	draw_admin();
	draw_log();
	draw_statusbar();
	/* ncurses pads don't reflow when wresized to a new width — existing
	   text keeps its old wrap points. the only way to get clean output
	   is to wipe the pad and re-render the full history from our own
	   log at the new width. */
	if (win_chat) {
		if (chat_log_n > 0)
			chat_log_replay();
		else
			tui_draw_welcome(header_model[0] ? header_model : NULL);
	}
	draw_input_box();
	doupdate();
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
	/* raw() instead of cbreak() so Ctrl-C arrives as byte 3 in wgetch
	   rather than being intercepted by the tty layer as SIGINT. we want
	   the input loop to convert it into a "\x01exit" submission so the
	   main-loop /exit handler can prompt before killing an in-flight
	   model download. trade-off: Ctrl-Z no longer suspends to the shell
	   and Ctrl-S/Ctrl-Q flow-control are disabled — neither matters for
	   a chat TUI. */
	raw();
	noecho();
	keypad(stdscr, TRUE);

	if (has_colors()) {
		start_color();
		use_default_colors();
		colors_available = 1;

		if (can_change_color() && COLORS >= 28) {
			init_color(KAI_BLACK,        86,   86,   86);   /* #161616 */
			init_color(KAI_RED,         910,  333,  333);   /* #e85555 */
			init_color(KAI_GREEN,       416,  690,  314);   /* #6ab050 */
			init_color(KAI_ORANGE,      890,  529,  208);   /* #e38735 */
			init_color(KAI_BLUE,        361,  553,  800);   /* #5c8dcc */
			init_color(KAI_MAGENTA,     588,  392,  863);   /* #9664dc */
			init_color(KAI_CYAN,        314,  745,  706);   /* #50beb4 */
			init_color(KAI_WHITE,       784,  784,  784);   /* #c8c8c8 */
			init_color(KAI_DIM,         333,  333,  333);   /* #555555 */
			init_color(KAI_BRIGHTWHITE, 933,  933,  933);   /* #eeeeee */
			init_color(KAI_HIGHLIGHT,   165,  165,  165);   /* #2a2a2a */
			init_color(KAI_BORDER,      200,  200,  200);   /* #333333 */
			init_color(KAI_PROSE_BG,    102,  102,  102);   /* #1a1a1a */

			init_pair(CP_FG,          KAI_WHITE,       -1);
			init_pair(CP_ACCENT,      KAI_ORANGE,      -1);
			init_pair(CP_DIM,         KAI_DIM,         -1);
			init_pair(CP_BORDER,      KAI_DIM,         -1);  /* #555 — visible on dark bg */
			init_pair(CP_SELECTED,    KAI_BLACK,       KAI_ORANGE);
			init_pair(CP_USER,        KAI_ORANGE,      -1);
			init_pair(CP_ASSISTANT,   KAI_WHITE,       -1);
			init_pair(CP_DAEMON_UP,   KAI_GREEN,       -1);
			init_pair(CP_DAEMON_DOWN, KAI_RED,         -1);
			init_pair(CP_MD_HEADING,    KAI_MAGENTA,   KAI_PROSE_BG);
			init_pair(CP_MD_CODE_INLINE, KAI_CYAN,     KAI_PROSE_BG);
			init_pair(CP_MD_CODE_BLOCK, KAI_CYAN,      KAI_HIGHLIGHT);
			init_pair(CP_MD_QUOTE,      KAI_DIM,       KAI_PROSE_BG);
			init_pair(CP_MD_PROSE,      KAI_WHITE,     KAI_PROSE_BG);
		} else {
			/* fallback: use standard 16-color slots */
			init_pair(CP_FG,          COLOR_WHITE,   -1);
			init_pair(CP_ACCENT,      COLOR_YELLOW,  -1);
			init_pair(CP_DIM,         8,             -1);
			init_pair(CP_BORDER,      8,             -1);
			init_pair(CP_SELECTED,    COLOR_BLACK,   COLOR_YELLOW);
			init_pair(CP_USER,        COLOR_YELLOW,  -1);
			init_pair(CP_ASSISTANT,   COLOR_WHITE,   -1);
			init_pair(CP_DAEMON_UP,   COLOR_GREEN,   -1);
			init_pair(CP_DAEMON_DOWN, COLOR_RED,     -1);
			/* fallback terminals can't define custom colours — so the prose
			   bg stays at terminal default. code blocks still get a visible
			   tint via ANSI 8 (bright black / gray). */
			init_pair(CP_MD_HEADING,    COLOR_MAGENTA, -1);
			init_pair(CP_MD_CODE_INLINE, COLOR_CYAN,   -1);
			init_pair(CP_MD_CODE_BLOCK, COLOR_CYAN,    8);
			init_pair(CP_MD_QUOTE,      8,             -1);
			init_pair(CP_MD_PROSE,      COLOR_WHITE,   -1);
		}
		user_msg_attr = A_BOLD | COLOR_PAIR(CP_USER);
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
	draw_log();   /* render the empty Log title so the pane isn't blank */
	statusbar_set_left(NULL);   /* NULL ⇒ draw_statusbar uses focus-default */
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
	if (win_sessions) { delwin(win_sessions); win_sessions = NULL; }
	if (win_admin)    { delwin(win_admin);    win_admin = NULL;    }
	if (win_log)      { delwin(win_log);      win_log = NULL;      }
	if (win_input) { delwin(win_input); win_input = NULL; }
	if (win_status) { delwin(win_status); win_status = NULL; }

	/* free any pending log entries */
	pthread_mutex_lock(&log_lock);
	for (int i = 0; i < LOG_RING_SIZE; i++) {
		free(log_ring[i].text);
		log_ring[i].text = NULL;
	}
	log_head = 0;
	log_count = 0;
	pthread_mutex_unlock(&log_lock);

	free(sessions_rows);
	sessions_rows = NULL;
	sessions_rows_n = 0;
	sessions_active_id = -1;

	free(model_rows);
	model_rows = NULL;
	model_rows_n = 0;

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

void tui_set_session_name(const char *name)
{
	snprintf(header_session, sizeof header_session, "%s", name ? name : "");
	if (initialized) {
		draw_header();
		draw_input_box();
	}
}

void tui_set_daemon_status(int up)
{
	daemon_status = up ? 1 : 0;
	if (initialized) {
		draw_header();
		draw_input_box();
	}
}

/* thread-safe: stash the new status; main loop picks it up on next tick.
   safe to call from any thread (including signal handlers). */
void tui_post_daemon_status(int up)
{
	pending_daemon_status = up ? 1 : 0;
}

void tui_set_cancellable(int yn)
{
	cancellable_active = yn ? 1 : 0;
}

void tui_post_models_refresh(void)
{
	pending_models_refresh = 1;
}

void tui_set_models_refresh_cb(void (*cb)(void))
{
	models_refresh_cb = cb;
}

/* called on the main thread from tui_input's poll loop */
static void service_pending_daemon_status(void)
{
	int p = pending_daemon_status;
	if (p < 0) return;
	pending_daemon_status = -1;
	if (p != daemon_status) {
		daemon_status = p;
		if (initialized) {
			draw_header();
			draw_input_box();
			doupdate();
		}
	}
}

void tui_draw(void)
{
	clear();
	refresh();
	layout_windows();
	draw_header();
	draw_sessions();
	draw_admin();
	draw_log();
	if (win_chat) {
		werase(win_chat);
		/* full repaint = new session / session switch; the caller is
		   about to replay messages via tui_user_msg / tui_assistant_*,
		   so drop any stale log entries before they do. */
		chat_log_clear();
		assistant_pending_reset();
		refresh_chat();
	}
	draw_statusbar();
	draw_input_box();  /* must be last — owns the cursor */
}

static void apply_focus(int focus)
{
	tui_focus = focus;
	if (initialized) {
		curs_set(tui_focus == FOCUS_CHAT ? 1 : 0);
		draw_sessions();
		draw_admin();
		draw_statusbar();
		draw_input_box();
		doupdate();
	}
}
void tui_clear_chat(void)
{
	chat_log_clear();
	assistant_pending_reset();
	if (win_chat) werase(win_chat);
	chat_scroll = 0;
	refresh_chat();
}
void tui_focus_chat(void)     { apply_focus(FOCUS_CHAT); }
void tui_focus_sessions(void) { apply_focus(FOCUS_SESSIONS); }
void tui_focus_admin(void)    { apply_focus(FOCUS_ADMIN); }

void tui_toggle_sessions(void)
{
	sessions_visible = !sessions_visible;
	layout_windows();
	draw_header();
	draw_sessions();
	draw_admin();
	draw_log();
	if (win_chat) {
		werase(win_chat);
		if (chat_log_n > 0)
			chat_log_replay();
		else
			tui_draw_welcome(header_model[0] ? header_model : NULL);
	}
	draw_statusbar();
	draw_input_box();
	doupdate();
}

void tui_toggle_models(void)
{
	admin_visible = !admin_visible;
	layout_windows();
	draw_header();
	draw_sessions();
	draw_admin();
	draw_log();
	if (win_chat) {
		werase(win_chat);
		if (chat_log_n > 0)
			chat_log_replay();
		else
			tui_draw_welcome(header_model[0] ? header_model : NULL);
	}
	draw_statusbar();
	draw_input_box();
	doupdate();
}

int tui_highlighted_session_id(void)
{
	if (sessions_rows_n <= 0) return -1;
	int idx = clamp_hl(sessions_hl_idx, sessions_rows_n);
	return sessions_rows[idx].id;
}

const char *tui_highlighted_model_alias(void)
{
	int eff_n = admin_effective_n();
	if (eff_n <= 0) return NULL;
	int idx = clamp_hl(models_hl_idx, eff_n);
	if (synth_row_active()) {
		if (idx == 0) return synth_row.alias;
		return model_rows[idx - 1].alias;
	}
	return model_rows[idx].alias;
}

void tui_set_models(const struct tui_model_row *list, int n)
{
	free(model_rows);
	model_rows = NULL;
	model_rows_n = 0;

	if (n > 0 && list) {
		model_rows = calloc((size_t)n, sizeof *model_rows);
		if (model_rows) {
			memcpy(model_rows, list, (size_t)n * sizeof *model_rows);
			model_rows_n = n;
		}
	}

	if (initialized) {
		/* re-run layout so MODELS gets the right height for the new
		   row count — layout_windows sizes the pane against
		   model_rows_n. without this, the first tui_set_models after
		   init would leave MODELS stuck at its min height (5) until
		   the next resize or tui_draw. */
		layout_windows();
		draw_sessions();
		draw_admin();
		draw_log();
		/* layout_windows does erase()+refresh() on stdscr, which clears
		   the chat region on the physical screen. re-push the chat pad
		   so the welcome / conversation stays visible. */
		refresh_chat();
		draw_input_box();   /* restores cursor */
		doupdate();
	}
}

void tui_set_downloading_alias(const char *alias)
{
	pthread_mutex_lock(&downloading_lock);
	if (alias && *alias) {
		snprintf(downloading_alias, sizeof downloading_alias, "%s", alias);
	} else {
		downloading_alias[0] = '\0';
	}
	pthread_mutex_unlock(&downloading_lock);
	/* piggy-back on the existing "refresh models pane" post so the main
	   loop redraws draw_admin on its next tick — safe from any thread. */
	tui_post_models_refresh();
}

/* wake flag — set by bg workers via tui_post_wake(), cleared inside
   tui_input's poll loop when the buffer is empty. */
static volatile int wake_requested = 0;

void tui_post_wake(void)
{
	wake_requested = 1;
}

void tui_set_queued(const char *preview)
{
	/* sanitize: collapse whitespace so the preview stays on one line,
	   drop other control bytes, truncate to the buffer size. draw_statusbar
	   appends the result to the hint text when non-empty. */
	if (preview && *preview) {
		size_t n = 0;
		for (size_t i = 0; preview[i] && n < sizeof queued_preview - 1; i++) {
			unsigned char c = (unsigned char)preview[i];
			if (c == '\n' || c == '\r' || c == '\t')
				queued_preview[n++] = ' ';
			else if (c < 32)
				continue;
			else
				queued_preview[n++] = (char)c;
		}
		queued_preview[n] = '\0';
	} else {
		queued_preview[0] = '\0';
	}
	draw_statusbar();
	draw_input_box();   /* restores cursor + flushes the frame */
	doupdate();
}

void tui_set_sessions(const struct db_session *list, int n, int active_id)
{
	free(sessions_rows);
	sessions_rows = NULL;
	sessions_rows_n = 0;

	if (n > 0 && list) {
		sessions_rows = calloc((size_t)n, sizeof *sessions_rows);
		if (sessions_rows) {
			for (int i = 0; i < n; i++) {
				sessions_rows[i].id = list[i].id;
				sessions_rows[i].msg_count = list[i].message_count;
				snprintf(sessions_rows[i].name, sizeof sessions_rows[i].name,
				         "%s", list[i].name[0] ? list[i].name : "New session");
			}
			sessions_rows_n = n;
		}
	}
	sessions_active_id = active_id;

	if (initialized) {
		draw_sessions();
		draw_input_box();  /* restore cursor */
		doupdate();
	}
}

void tui_input_clear(void)
{
	input_clear();
	input_scroll = 0;
	if (input_h != INPUT_H_MIN) {
		input_h = INPUT_H_MIN;
		layout_windows();
		draw_header();
		draw_sessions();
		draw_admin();
		draw_log();
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
	wprintw(win_status, " [y]es / [n]o ");
	wattroff(win_status, A_DIM);
	wrefresh(win_status);

	/* enable keypad so arrow keys / function keys come through as KEY_*
	   constants instead of raw ESC bytes (which would be misread as "no").
	   also flush any stray input that arrived during model generation so
	   accidental keypresses don't instantly answer for the user. */
	keypad(win_status, TRUE);
	flushinp();

	/* wait for an explicit y/n keypress; ignore everything else.
	   Ctrl-C still works as a deny shortcut. */
	curs_set(0);
	while (1) {
		ch = wgetch(win_status);
		if (ch == ERR) continue;
		if (ch == 'y' || ch == 'Y') { ch = 'y'; break; }
		if (ch == 'n' || ch == 'N') { ch = 'n'; break; }
		if (ch == 3)                { ch = 'n'; break; }  /* Ctrl-C = no */
		/* ignore arrow keys, function keys, ESC, mouse, resize, etc */
	}

	/* restore normal statusbar */
	draw_statusbar();
	draw_input_box();
	return ch;
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

void tui_set_markdown(int enabled)
{
	markdown_enabled = enabled ? 1 : 0;
}

void tui_statusbar(const char *text)
{
	struct tui_event ev = {0};
	ev.type = TUI_EV_STATUSBAR_LEFT;
	ev.data = text ? strdup(text) : NULL;
	event_push(&ev);
	free(ev.data);
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

	curs_set(tui_focus == FOCUS_CHAT ? 1 : 0);
	draw_input_box();
	draw_statusbar();

	/* non-blocking: poll input every POLL_INTERVAL_MS,
	   drain event queue between polls */
	wtimeout(win_input, POLL_INTERVAL_MS);

	while (1) {
		/* drain pending events from background threads */
		drain_events();

		/* service deferred async things on the main thread */
		if (resize_pending)                service_resize();
		service_pending_daemon_status();
		if (pending_models_refresh) {
			pending_models_refresh = 0;
			if (models_refresh_cb) models_refresh_cb();
		}

		/* if a bg worker appended to the log, redraw that pane. done on
		   the poll tick so tui_log stays cheap for callers — they just
		   flip the flag and move on. */
		if (log_dirty) {
			log_dirty = 0;
			draw_log();
			draw_input_box();
			doupdate();
		}

		/* wake request from a finished bg worker (normally gen_thread_fn
		   when a generation turn just ended). only fire if the user isn't
		   actively editing — otherwise we'd discard their in-flight line. */
		if (wake_requested && input_len == 0) {
			wake_requested = 0;
			wtimeout(win_input, -1);
			curs_set(0);
			return strdup("\x02wake");
		}

		ch = wgetch(win_input);

		if (ch == ERR)
			continue;  /* timeout, no input — loop and drain again */

		/* ncurses reports terminal resize via KEY_RESIZE in addition
		   to SIGWINCH. service either path. */
		if (ch == KEY_RESIZE) {
			service_resize();
			continue;
		}

		/* --- global focus-switching keys (any pane) --- */
		if (ch == '\t' || ch == KEY_BTAB) {
			int dir = (ch == '\t') ? 1 : 2;  /* forward or backward (=+2 mod 3) */
			do {
				tui_focus = (tui_focus + dir) % 3;
			} while ((tui_focus == FOCUS_ADMIN && (!win_admin || !admin_visible)) ||
			         (tui_focus == FOCUS_SESSIONS && (!win_sessions || !sessions_visible)));
			curs_set(tui_focus == FOCUS_CHAT ? 1 : 0);
			draw_sessions();
			draw_admin();
			draw_statusbar();
			draw_input_box();
			doupdate();
			continue;
		}

		/* Ctrl-C is global: works from any pane. without this, the
		   SESSIONS/ADMIN switch tables further down swallow it via
		   their trailing `continue`, which would block the download
		   cancel-and-quit prompt from ever being shown. */
		if (ch == 3) {
			wtimeout(win_input, -1);
			curs_set(0);
			return strdup("\x01" "exit");
		}

		/* ESC with an Alt-key follow-up: handle all globally-bound
		   ones here so they fire from every pane without being
		   swallowed by SESSIONS/ADMIN `continue`s. we intentionally
		   handle Alt+Enter here too (instead of deferring to CHAT's
		   own ESC handler via ungetch) — ncurses keypad translation
		   of a re-pushed \r can misclassify it on round-trip, so
		   doing it inline is more reliable.

		   plain ESC (no follow-up) and bracketed-paste (ESC [ 200 ~)
		   still need the CHAT-pane handler further down, so we only
		   forward those via ungetch when focus is CHAT. */
		if (ch == 27) {
			wtimeout(win_input, 50);
			int next = wgetch(win_input);
			wtimeout(win_input, POLL_INTERVAL_MS);

			if (next == 'h' || next == 'H') {
				wtimeout(win_input, -1); curs_set(0);
				return strdup("\x01" "help");
			}
			if ((next == 'j' || next == 'k') && win_chat) {
				int cy, cx; getyx(win_chat, cy, cx); (void)cx;
				int max_scroll = cy - chat_h + 1;
				if (max_scroll < 0) max_scroll = 0;
				if (next == 'j') {
					chat_scroll += 3;
					if (chat_scroll > max_scroll)
						chat_scroll = max_scroll;
				} else {
					chat_scroll -= 3;
					if (chat_scroll < 0) chat_scroll = 0;
				}
				refresh_chat();
				draw_input_box();
				doupdate();
				continue;
			}
			if (next == '\n' || next == '\r' || next == KEY_ENTER) {
				/* Alt+Enter: insert a literal newline into the input
				   buffer. only meaningful when CHAT is focused. */
				if (tui_focus == FOCUS_CHAT) {
					input_insert_char('\n');
					recalc_input_height();
					update_dynamic_statusbar();
					draw_input_box();
					doupdate();
				}
				continue;
			}
			if (next == '[' && tui_focus == FOCUS_CHAT) {
				/* CSI sequence (bracketed paste etc.) — let the
				   CHAT pane's own case 27 handle the full parse. */
				ungetch(next);
				/* fall through with ch == 27 */
			} else {
				/* plain Esc or unknown Alt: swallow for non-CHAT,
				   fall through to CHAT's case 27 for the cancel path. */
				if (tui_focus != FOCUS_CHAT) continue;
				if (next != ERR) ungetch(next);
			}
		}

		/* Alt+S / Alt+M: toggle sessions or models pane (global) */
		if (ch == 27) {
			wtimeout(win_input, 15);
			int next = wgetch(win_input);
			wtimeout(win_input, POLL_INTERVAL_MS);
			if (next == 's' || next == 'S') {
				tui_toggle_sessions();
				continue;
			}
			if (next == 'm' || next == 'M') {
				tui_toggle_models();
				continue;
			}
			/* global Alt+j / Alt+k: scroll chat back/forward */
			if ((next == 'j' || next == 'k') && win_chat && chat_log_n > 0) {
				int cur_y;
				getyx(win_chat, cur_y, (int){0});
				int max_scroll = cur_y - chat_h + 1;
				if (max_scroll < 0) max_scroll = 0;
				if (next == 'j') {
					chat_scroll += 3;
					if (chat_scroll > max_scroll) chat_scroll = max_scroll;
				} else {
					chat_scroll -= 3;
					if (chat_scroll < 0) chat_scroll = 0;
				}
				refresh_chat();
				draw_input_box();
				doupdate();
				continue;
			}
		}

		/* --- SESSIONS pane focused ---
		   j / k directions mirror kc (config.h): j = up, k = down. */
		if (tui_focus == FOCUS_SESSIONS) {
			switch (ch) {
			case 'k': case KEY_DOWN:
				if (sessions_rows_n > 0)
					sessions_hl_idx = (sessions_hl_idx + 1) % sessions_rows_n;
				draw_sessions(); draw_input_box(); doupdate();
				continue;
			case 'j': case KEY_UP:
				if (sessions_rows_n > 0)
					sessions_hl_idx = (sessions_hl_idx - 1 + sessions_rows_n) % sessions_rows_n;
				draw_sessions(); draw_input_box(); doupdate();
				continue;
			case '\n': case '\r': case KEY_ENTER:
				wtimeout(win_input, -1); curs_set(0); return strdup("\x01" "open");
			case 'n':
				wtimeout(win_input, -1); curs_set(0); return strdup("\x01" "new");
			case 'd':
				wtimeout(win_input, -1); curs_set(0); return strdup("\x01" "delete");
			case 'r': {
				static const char *pfx = "\x01" "rename ";
				tui_focus = FOCUS_CHAT;
				input_clear();
				for (const char *p = pfx; *p; p++) input_insert_char(*p);
				recalc_input_height();
				curs_set(1);
				draw_sessions(); draw_admin(); draw_statusbar(); draw_input_box();
				doupdate();
				continue;
			}
			case '?':
				wtimeout(win_input, -1); curs_set(0); return strdup("\x01" "help");
			}
			continue;
		}

		/* --- ADMIN pane focused --- (same j=up / k=down as kc) */
		if (tui_focus == FOCUS_ADMIN) {
			int eff_n = admin_effective_n();
			switch (ch) {
			case 'k': case KEY_DOWN:
				if (eff_n > 0)
					models_hl_idx = (models_hl_idx + 1) % eff_n;
				/* redraw statusbar too: the 'x' hint flips between
				   "rm" and "cancel" depending on whether the now-
				   highlighted row is the one currently pulling. */
				draw_admin(); draw_statusbar(); draw_input_box(); doupdate();
				continue;
			case 'j': case KEY_UP:
				if (eff_n > 0)
					models_hl_idx = (models_hl_idx - 1 + eff_n) % eff_n;
				draw_admin(); draw_statusbar(); draw_input_box(); doupdate();
				continue;
			case '\n': case '\r': case KEY_ENTER:
				wtimeout(win_input, -1); curs_set(0); return strdup("\x01" "model_switch");
			case 'p':
				wtimeout(win_input, -1); curs_set(0); return strdup("\x01" "model_pull");
			case 'd': {
				/* context-sensitive: cancel the pull if the highlighted
				   row is the one currently downloading, otherwise fall
				   through to the normal "remove from disk" handler.
				   'd' matches the SESSIONS-pane delete gesture so both
				   panes share the same muscle memory for destructive
				   actions. */
				const char *hl = tui_highlighted_model_alias();
				wtimeout(win_input, -1); curs_set(0);
				if (hl && row_is_downloading(hl))
					return strdup("\x01" "model_cancel");
				return strdup("\x01" "model_rm");
			}
			case 'u': {
				/* pre-fill the input box with `/pull ` so the user can
				   paste or type an arbitrary URL and hit Enter. same
				   pattern as SESSIONS 'r' for rename. */
				static const char *pfx = "\x01" "pull ";
				tui_focus = FOCUS_CHAT;
				input_clear();
				for (const char *p = pfx; *p; p++) input_insert_char(*p);
				recalc_input_height();
				curs_set(1);
				draw_sessions(); draw_admin(); draw_statusbar(); draw_input_box();
				doupdate();
				continue;
			}
			}
			continue;
		}

		/* --- CHAT pane focused: fall through to the text-editing switch --- */

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
				draw_sessions();
				draw_admin();
				draw_log();
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
			/* 25ms matches kc's set_escdelay - more reliable for Alt+key */
			wtimeout(win_input, 25);
			next = wgetch(win_input);
			wtimeout(win_input, POLL_INTERVAL_MS);

			if (next == '\n' || next == '\r' || next == KEY_ENTER) {
				/* Alt+Enter: insert newline */
				input_insert_char('\n');
				recalc_input_height();
				update_dynamic_statusbar();
				break;
			}

			/* Note: Alt+h, Alt+j, Alt+k are handled in the global
			   block above this switch so they work from every pane.
			   what lands here is the leftover CHAT-only interactions
			   (Alt+Enter, bracketed paste, plain Esc). */

			if (next == '[') {
				/* could be bracketed paste: ESC [ 2 0 0 ~ */
				int seq[4];
				int si = 0;
				/* slightly longer for the 4-byte CSI tail (slow pastes) */
				wtimeout(win_input, 40);
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
				/* plain ESC:
				   - if a generation / compaction is in flight, cancel
				   - if a command entry is in progress (buffer has the
				     \x01 marker from r/m/p), abandon it: clear the
				     buffer and hop focus back to where it came from
				   - otherwise no-op */
				if (cancellable_active) {
					wtimeout(win_input, -1);
					curs_set(0);
					return strdup("");
				}
				if (input_len > 0 &&
				    (unsigned char)input_buf[0] == 1) {
					input_clear();
					recalc_input_height();
					tui_focus = FOCUS_SESSIONS;
					curs_set(0);
					draw_sessions();
					draw_admin();
					draw_statusbar();
					draw_input_box();
				}
				break;
			}
			/* other Alt+key: ignore */
			break;
		}

		case KEY_BACKSPACE:
		case 127:
		case 8:
			if (input_pos > 0) {
				/* preserve the hidden command-mode marker at pos 0.
				   deleting it would drop the user into "this is a chat
				   message" interpretation mid-typing, which is surprising. */
				if (input_pos == 1 && (unsigned char)input_buf[0] == 1)
					break;
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

		case 11:  /* Ctrl-K: explicit no-op (was bubbling up to the
		             terminal / tmux and causing UI weirdness). */
			break;

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

		case 12:  /* Ctrl-L: clear chat window */
			if (win_chat) {
				werase(win_chat);
				chat_scroll_bottom();
				refresh_chat();
				draw_input_box();
			}
			break;

		/* chat scrolling is via mouse wheel or (future) keybinds inside
		   CHAT focus; Page Up/Down and Ctrl-based nav shortcuts were
		   removed in favour of the Tab-focused pane model. */

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

	/* wipe the pad first — this is called from the resize path too, and
	   without a clear the logo would stack on top of the previous one
	   every time the terminal is resized while the session is empty. */
	werase(win_chat);
	wmove(win_chat, 0, 0);

	wprintw(win_chat, "\n");
	draw_logo();
	wprintw(win_chat, "\n");

	wattron(win_chat, A_DIM);
	wprintw(win_chat, "  v%s", VERSION);
	if (model && model[0])
		wprintw(win_chat, " | %s", model);
	wprintw(win_chat, "\n");
	wprintw(win_chat, "  Type a message to start.\n");
	wattroff(win_chat, A_DIM);

	refresh_chat();
	doupdate();
}
