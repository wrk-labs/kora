#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "input.h"

/* original terminal settings */
static struct termios orig_termios;
static int raw_mode = 0;

/* dynamic history */
static char **history = NULL;
static int history_count = 0;
static int history_cap = 0;

void input_init(void)
{
	if (tcgetattr(STDIN_FILENO, &orig_termios) == 0)
		raw_mode = 1;
}

void input_cleanup(void)
{
	if (raw_mode)
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);

	int i;
	for (i = 0; i < history_count; i++)
		free(history[i]);
	free(history);
	history = NULL;
	history_count = 0;
	history_cap = 0;
}

static void history_add(const char *line)
{
	if (!line || line[0] == '\0')
		return;

	/* skip duplicates of the last entry */
	if (history_count > 0 && strcmp(history[history_count - 1], line) == 0)
		return;

	if (history_count >= history_cap) {
		int new_cap = history_cap == 0 ? 64 : history_cap * 2;
		char **new_hist = realloc(history, new_cap * sizeof(char *));
		if (!new_hist)
			return;
		history = new_hist;
		history_cap = new_cap;
	}

	history[history_count] = strdup(line);
	if (history[history_count])
		history_count++;
}

static void enable_raw(void)
{
	if (!raw_mode)
		return;
	struct termios raw = orig_termios;
	raw.c_lflag &= ~(ECHO | ICANON | ISIG);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static void disable_raw(void)
{
	if (!raw_mode)
		return;
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

/* ensure buf has room for at least need+1 bytes */
static char *buf_grow(char *buf, size_t *cap, size_t need)
{
	if (need + 1 <= *cap)
		return buf;
	size_t new_cap = *cap == 0 ? 128 : *cap;
	while (new_cap <= need)
		new_cap *= 2;
	char *new_buf = realloc(buf, new_cap);
	if (!new_buf)
		return buf;
	*cap = new_cap;
	return new_buf;
}

/* clear current line on screen and redraw */
static void refresh_line(const char *prompt, const char *buf, size_t len, size_t pos)
{
	/* move to start of line, clear it */
	printf("\r\033[K%s%.*s", prompt, (int)len, buf);

	/* move cursor to correct position */
	size_t prompt_len = strlen(prompt);
	if (pos < len) {
		int back = (int)(len - pos);
		printf("\033[%dD", back);
	}
	(void)prompt_len;
	fflush(stdout);
}

char *input_read(const char *prompt)
{
	if (!raw_mode) {
		/* fallback: no terminal, use fgets */
		printf("%s", prompt);
		fflush(stdout);
		char tmp[4096];
		if (!fgets(tmp, sizeof(tmp), stdin))
			return NULL;
		tmp[strcspn(tmp, "\n")] = 0;
		history_add(tmp);
		return strdup(tmp);
	}

	enable_raw();

	char *buf = NULL;
	size_t cap = 0;
	size_t len = 0;
	size_t pos = 0;
	int hist_idx = history_count;
	char *saved_line = NULL; /* saves current input when browsing history */

	buf = buf_grow(buf, &cap, 128);
	buf[0] = '\0';

	printf("%s", prompt);
	fflush(stdout);

	while (1) {
		char c;
		if (read(STDIN_FILENO, &c, 1) != 1) {
			/* EOF */
			free(buf);
			free(saved_line);
			disable_raw();
			return NULL;
		}

		if (c == '\r' || c == '\n') {
			printf("\n");
			break;
		}

		/* Ctrl-C */
		if (c == 3) {
			free(buf);
			free(saved_line);
			disable_raw();
			printf("\n");
			return strdup("");
		}

		/* Ctrl-D on empty line = EOF */
		if (c == 4 && len == 0) {
			free(buf);
			free(saved_line);
			disable_raw();
			return NULL;
		}

		/* backspace */
		if (c == 127 || c == 8) {
			if (pos > 0) {
				memmove(buf + pos - 1, buf + pos, len - pos);
				pos--;
				len--;
				buf[len] = '\0';
				refresh_line(prompt, buf, len, pos);
			}
			continue;
		}

		/* escape sequences */
		if (c == 27) {
			char seq[2];
			if (read(STDIN_FILENO, &seq[0], 1) != 1)
				continue;
			if (read(STDIN_FILENO, &seq[1], 1) != 1)
				continue;

			if (seq[0] == '[') {
				/* up arrow */
				if (seq[1] == 'A') {
					if (history_count == 0)
						continue;
					if (hist_idx == history_count) {
						free(saved_line);
						saved_line = strdup(buf);
					}
					if (hist_idx > 0) {
						hist_idx--;
						len = strlen(history[hist_idx]);
						buf = buf_grow(buf, &cap, len);
						memcpy(buf, history[hist_idx], len + 1);
						pos = len;
						refresh_line(prompt, buf, len, pos);
					}
					continue;
				}

				/* down arrow */
				if (seq[1] == 'B') {
					if (hist_idx < history_count) {
						hist_idx++;
						if (hist_idx == history_count && saved_line) {
							len = strlen(saved_line);
							buf = buf_grow(buf, &cap, len);
							memcpy(buf, saved_line, len + 1);
						} else if (hist_idx < history_count) {
							len = strlen(history[hist_idx]);
							buf = buf_grow(buf, &cap, len);
							memcpy(buf, history[hist_idx], len + 1);
						}
						pos = len;
						refresh_line(prompt, buf, len, pos);
					}
					continue;
				}

				/* right arrow */
				if (seq[1] == 'C') {
					if (pos < len) {
						pos++;
						printf("\033[1C");
						fflush(stdout);
					}
					continue;
				}

				/* left arrow */
				if (seq[1] == 'D') {
					if (pos > 0) {
						pos--;
						printf("\033[1D");
						fflush(stdout);
					}
					continue;
				}

				/* home */
				if (seq[1] == 'H') {
					pos = 0;
					refresh_line(prompt, buf, len, pos);
					continue;
				}

				/* end */
				if (seq[1] == 'F') {
					pos = len;
					refresh_line(prompt, buf, len, pos);
					continue;
				}

				/* delete key: ESC [ 3 ~ */
				if (seq[1] == '3') {
					char tilde;
					if (read(STDIN_FILENO, &tilde, 1) == 1 && tilde == '~') {
						if (pos < len) {
							memmove(buf + pos, buf + pos + 1, len - pos - 1);
							len--;
							buf[len] = '\0';
							refresh_line(prompt, buf, len, pos);
						}
					}
					continue;
				}
			}
			continue;
		}

		/* Ctrl-A: home */
		if (c == 1) {
			pos = 0;
			refresh_line(prompt, buf, len, pos);
			continue;
		}

		/* Ctrl-E: end */
		if (c == 5) {
			pos = len;
			refresh_line(prompt, buf, len, pos);
			continue;
		}

		/* Ctrl-U: clear line */
		if (c == 21) {
			len = 0;
			pos = 0;
			buf[0] = '\0';
			refresh_line(prompt, buf, len, pos);
			continue;
		}

		/* Ctrl-L: clear screen */
		if (c == 12) {
			printf("\033[2J\033[H");
			refresh_line(prompt, buf, len, pos);
			continue;
		}

		/* Ctrl-W: delete word backwards */
		if (c == 23) {
			if (pos > 0) {
				size_t old_pos = pos;
				while (pos > 0 && buf[pos - 1] == ' ')
					pos--;
				while (pos > 0 && buf[pos - 1] != ' ')
					pos--;
				memmove(buf + pos, buf + old_pos, len - old_pos);
				len -= (old_pos - pos);
				buf[len] = '\0';
				refresh_line(prompt, buf, len, pos);
			}
			continue;
		}

		/* regular character */
		if (c >= 32) {
			buf = buf_grow(buf, &cap, len + 1);
			if (pos < len)
				memmove(buf + pos + 1, buf + pos, len - pos);
			buf[pos] = c;
			pos++;
			len++;
			buf[len] = '\0';
			refresh_line(prompt, buf, len, pos);
		}
	}

	disable_raw();
	free(saved_line);

	buf[len] = '\0';
	history_add(buf);
	return buf;
}
