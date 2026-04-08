#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>

#include "parser.h"

void kora_parser_init(struct kora_parser *p)
{
	memset(p, 0, sizeof(*p));
	p->buf_cap = 4096;
	p->buf = malloc(p->buf_cap);
	if (p->buf)
		p->buf[0] = '\0';
}

void kora_parser_free(struct kora_parser *p)
{
	if (!p)
		return;
	free(p->buf);
	free(p->call_body);
	memset(p, 0, sizeof(*p));
}

void kora_parser_reset(struct kora_parser *p)
{
	p->buf_len = 0;
	if (p->buf)
		p->buf[0] = '\0';
	free(p->call_body);
	p->call_body = NULL;
	p->call_body_len = 0;
	p->text_end = 0;
	p->state = KORA_PARSE_TEXT;
}

static int ensure_cap(struct kora_parser *p, size_t need)
{
	if (need <= p->buf_cap)
		return 0;
	while (p->buf_cap < need)
		p->buf_cap *= 2;
	char *nb = realloc(p->buf, p->buf_cap);
	if (!nb)
		return -1;
	p->buf = nb;
	return 0;
}

/* find substring within p->buf starting at `from`. returns offset or (size_t)-1. */
static size_t find_substr(const char *hay, size_t hay_len, size_t from, const char *needle)
{
	size_t nlen = strlen(needle);
	if (nlen == 0 || from + nlen > hay_len)
		return (size_t)-1;
	const char *p = memmem(hay + from, hay_len - from, needle, nlen);
	if (!p)
		return (size_t)-1;
	return (size_t)(p - hay);
}

int kora_parser_feed(struct kora_parser *p, const char *text, int len)
{
	if (len <= 0 || !p->buf)
		return 0;

	if (ensure_cap(p, p->buf_len + len + 1) < 0)
		return 0;
	memcpy(p->buf + p->buf_len, text, len);
	p->buf_len += len;
	p->buf[p->buf_len] = '\0';

	if (p->state == KORA_PARSE_DONE)
		return 1;

	if (p->state == KORA_PARSE_TEXT) {
		size_t pos = find_substr(p->buf, p->buf_len, 0, KORA_TC_OPEN);
		if (pos == (size_t)-1)
			return 0;
		p->text_end = pos;
		p->state = KORA_PARSE_INSIDE;
	}

	if (p->state == KORA_PARSE_INSIDE) {
		size_t open_end = p->text_end + strlen(KORA_TC_OPEN);
		size_t close_pos = find_substr(p->buf, p->buf_len, open_end, KORA_TC_CLOSE);
		if (close_pos == (size_t)-1)
			return 0;

		size_t body_len = close_pos - open_end;
		p->call_body = malloc(body_len + 1);
		if (!p->call_body)
			return 0;
		memcpy(p->call_body, p->buf + open_end, body_len);
		p->call_body[body_len] = '\0';
		p->call_body_len = body_len;
		p->state = KORA_PARSE_DONE;
		return 1;
	}

	return 0;
}

int kora_parser_finalize(struct kora_parser *p)
{
	if (p->state == KORA_PARSE_DONE)
		return 1;
	if (p->state != KORA_PARSE_INSIDE)
		return 0;

	/* model emitted <tool_call> and a body but never wrote </tool_call>.
	   take everything from after <tool_call> through end-of-buf as the body,
	   stripping trailing whitespace. */
	size_t open_end = p->text_end + strlen(KORA_TC_OPEN);
	if (open_end > p->buf_len)
		return 0;
	size_t start = open_end;
	while (start < p->buf_len) {
		char c = p->buf[start];
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
			start++;
		else
			break;
	}
	size_t end = p->buf_len;
	while (end > start) {
		char c = p->buf[end - 1];
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
			end--;
		else
			break;
	}
	size_t body_len = end - start;
	p->call_body = malloc(body_len + 1);
	if (!p->call_body)
		return 0;
	memcpy(p->call_body, p->buf + start, body_len);
	p->call_body[body_len] = '\0';
	p->call_body_len = body_len;
	p->state = KORA_PARSE_DONE;
	return 1;
}

const char *kora_parser_text(struct kora_parser *p)
{
	if (!p->buf)
		return "";
	if (p->state == KORA_PARSE_TEXT) {
		return p->buf;
	}
	/* truncate at text_end so the caller sees only pre-tool-call text */
	if (p->text_end < p->buf_cap)
		p->buf[p->text_end] = '\0';
	return p->buf;
}
