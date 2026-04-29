#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>

#include "prompt.h"

static void lowercase_in_place(char *s)
{
	for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

static void fill_date(char *out, size_t cap)
{
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(out, cap, "%A, %B %d, %Y", &tm);
}

static void fill_time(char *out, size_t cap)
{
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	/* %z gives "-0300"; we break it into "-03:00"-ish by hand so the prompt
	   is easier for the model to read. keep it cheap — no tz db needed. */
	char hm[8], off[8];
	strftime(hm,  sizeof hm,  "%H:%M", &tm);
	strftime(off, sizeof off, "%z",    &tm);
	if (strlen(off) == 5) {
		char pretty[8];
		snprintf(pretty, sizeof pretty, "%c%c%c",
		         off[0], off[1], off[2]);
		snprintf(out, cap, "%s %s", hm, pretty);
	} else {
		snprintf(out, cap, "%s", hm);
	}
}

static void fill_platform(char *out, size_t cap)
{
	struct utsname u;
	if (uname(&u) == 0) {
		snprintf(out, cap, "%s", u.sysname);
		lowercase_in_place(out);
	} else {
		snprintf(out, cap, "(unknown)");
	}
}

/* Grow `buf` so at least `need` more bytes can be written at *pos without
   overflow. *cap is updated. Returns 0 on success, -1 on OOM. */
static int ensure(char **buf, size_t *cap, size_t pos, size_t need)
{
	if (pos + need + 1 <= *cap) return 0;
	size_t nc = *cap ? *cap : 256;
	while (nc < pos + need + 1) nc *= 2;
	char *nb = realloc(*buf, nc);
	if (!nb) return -1;
	*buf = nb;
	*cap = nc;
	return 0;
}

static int append(char **buf, size_t *cap, size_t *pos, const char *s, size_t n)
{
	if (ensure(buf, cap, *pos, n) != 0) return -1;
	memcpy(*buf + *pos, s, n);
	*pos += n;
	(*buf)[*pos] = '\0';
	return 0;
}

/* Match "{key}" starting at `src`. If it matches, writes the key into
   key_out (null-terminated, at most key_cap-1 chars) and returns the
   number of bytes consumed (including the braces). Returns 0 if no match. */
static size_t match_placeholder(const char *src, char *key_out, size_t key_cap)
{
	if (*src != '{') return 0;
	const char *end = strchr(src + 1, '}');
	if (!end) return 0;
	size_t klen = (size_t)(end - src - 1);
	if (klen == 0 || klen >= key_cap) return 0;
	/* only accept lowercase letters — keeps it unambiguous and means a
	   stray "{foo bar}" or "{x}" with odd chars falls through to literal. */
	for (size_t i = 0; i < klen; i++) {
		char c = src[1 + i];
		if (c < 'a' || c > 'z') return 0;
	}
	memcpy(key_out, src + 1, klen);
	key_out[klen] = '\0';
	return klen + 2;
}

char *kora_prompt_render(const char *tmpl, const char *model, int ctx_size)
{
	if (!tmpl) return NULL;

	char date_buf[64]     = {0};
	char time_buf[32]     = {0};
	char platform_buf[68] = {0};
	char ctx_buf[32]      = {0};
	const char *model_str = (model && *model) ? model : "(unknown)";

	fill_date(date_buf, sizeof date_buf);
	fill_time(time_buf, sizeof time_buf);
	fill_platform(platform_buf, sizeof platform_buf);
	if (ctx_size > 0) snprintf(ctx_buf, sizeof ctx_buf, "%d", ctx_size);
	else              snprintf(ctx_buf, sizeof ctx_buf, "(unknown)");

	size_t cap = 0, pos = 0;
	char *out = NULL;
	if (ensure(&out, &cap, 0, strlen(tmpl) + 128) != 0) return NULL;
	out[0] = '\0';

	const char *p = tmpl;
	while (*p) {
		char key[16];
		size_t consumed = match_placeholder(p, key, sizeof key);
		if (consumed == 0) {
			if (append(&out, &cap, &pos, p, 1) != 0) { free(out); return NULL; }
			p++;
			continue;
		}

		const char *val = NULL;
		if      (strcmp(key, "date")     == 0) val = date_buf;
		else if (strcmp(key, "time")     == 0) val = time_buf;
		else if (strcmp(key, "platform") == 0) val = platform_buf;
		else if (strcmp(key, "model")    == 0) val = model_str;
		else if (strcmp(key, "ctx")      == 0) val = ctx_buf;

		if (val) {
			if (append(&out, &cap, &pos, val, strlen(val)) != 0) {
				free(out);
				return NULL;
			}
			p += consumed;
		} else {
			/* unknown placeholder — copy the literal "{key}" through so
			   the user sees their typo instead of an empty gap. */
			if (append(&out, &cap, &pos, p, consumed) != 0) {
				free(out);
				return NULL;
			}
			p += consumed;
		}
	}

	return out;
}
