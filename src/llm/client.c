#define _GNU_SOURCE
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"

/* --- tiny string builder --- */

struct sb { char *buf; size_t len; size_t cap; };

static int sb_reserve(struct sb *s, size_t extra)
{
	if (s->len + extra + 1 <= s->cap) return 0;
	size_t ncap = s->cap ? s->cap : 128;
	while (ncap < s->len + extra + 1) ncap *= 2;
	char *nb = realloc(s->buf, ncap);
	if (!nb) return -1;
	s->buf = nb;
	s->cap = ncap;
	return 0;
}

static int sb_putc(struct sb *s, char c)
{
	if (sb_reserve(s, 1) < 0) return -1;
	s->buf[s->len++] = c;
	s->buf[s->len] = '\0';
	return 0;
}

static int sb_append(struct sb *s, const char *data, size_t n)
{
	if (sb_reserve(s, n) < 0) return -1;
	memcpy(s->buf + s->len, data, n);
	s->len += n;
	s->buf[s->len] = '\0';
	return 0;
}

static int sb_puts(struct sb *s, const char *str)
{
	return sb_append(s, str, strlen(str));
}

static int sb_puts_json_escaped(struct sb *s, const char *str)
{
	for (; *str; str++) {
		unsigned char c = (unsigned char)*str;
		int rc = 0;
		switch (c) {
		case '"':  rc = sb_puts(s, "\\\""); break;
		case '\\': rc = sb_puts(s, "\\\\"); break;
		case '\b': rc = sb_puts(s, "\\b");  break;
		case '\f': rc = sb_puts(s, "\\f");  break;
		case '\n': rc = sb_puts(s, "\\n");  break;
		case '\r': rc = sb_puts(s, "\\r");  break;
		case '\t': rc = sb_puts(s, "\\t");  break;
		default:
			if (c < 0x20) {
				char esc[8];
				snprintf(esc, sizeof esc, "\\u%04x", c);
				rc = sb_puts(s, esc);
			} else {
				rc = sb_putc(s, (char)c);
			}
		}
		if (rc < 0) return -1;
	}
	return 0;
}

/* --- JSON content extractor (best-effort, unescape-aware) --- */

/* given a pointer to a JSON object string, find the value of "content":"..."
   (the FIRST occurrence inside `choices[0].delta.content`). writes unescaped
   bytes into out; returns number of bytes written. -1 if not found. */
int kora_json_extract_content(const char *json, char *out, int out_cap)
{
	const char *p = strstr(json, "\"content\"");
	if (!p) return -1;
	p += 9;
	while (*p == ' ' || *p == ':') p++;
	if (*p != '"') return -1;
	p++;

	int n = 0;
	while (*p && n + 1 < out_cap) {
		if (*p == '"') break;
		if (*p == '\\') {
			p++;
			if (!*p) break;
			switch (*p) {
			case 'n': out[n++] = '\n'; p++; break;
			case 'r': out[n++] = '\r'; p++; break;
			case 't': out[n++] = '\t'; p++; break;
			case 'b': out[n++] = '\b'; p++; break;
			case 'f': out[n++] = '\f'; p++; break;
			case '"': out[n++] = '"';  p++; break;
			case '\\':out[n++] = '\\'; p++; break;
			case '/': out[n++] = '/';  p++; break;
			case 'u': {
				if (!p[1] || !p[2] || !p[3] || !p[4]) goto done;
				unsigned code = 0;
				for (int i = 1; i <= 4; i++) {
					int h;
					char d = p[i];
					if (d >= '0' && d <= '9') h = d - '0';
					else if (d >= 'a' && d <= 'f') h = 10 + d - 'a';
					else if (d >= 'A' && d <= 'F') h = 10 + d - 'A';
					else goto done;
					code = (code << 4) | h;
				}
				p += 5;
				if (code < 0x80) {
					if (n + 1 >= out_cap) goto done;
					out[n++] = (char)code;
				} else if (code < 0x800) {
					if (n + 2 >= out_cap) goto done;
					out[n++] = (char)(0xC0 | (code >> 6));
					out[n++] = (char)(0x80 | (code & 0x3F));
				} else {
					if (n + 3 >= out_cap) goto done;
					out[n++] = (char)(0xE0 | (code >> 12));
					out[n++] = (char)(0x80 | ((code >> 6) & 0x3F));
					out[n++] = (char)(0x80 | (code & 0x3F));
				}
				break;
			}
			default: out[n++] = *p++; break;
			}
		} else {
			out[n++] = *p++;
		}
	}
done:
	out[n] = '\0';
	return n;
}

/* --- request body builders --- */

static char *build_chat_body(const struct kora_client_chat_opts *opts)
{
	struct sb s = {0};
	if (sb_puts(&s, "{\"model\":\"") < 0) goto fail;
	if (sb_puts_json_escaped(&s, opts->model) < 0) goto fail;
	if (sb_puts(&s, "\",\"stream\":true,\"messages\":[") < 0) goto fail;
	for (int i = 0; i < opts->n_msgs; i++) {
		if (i && sb_putc(&s, ',') < 0) goto fail;
		if (sb_puts(&s, "{\"role\":\"") < 0) goto fail;
		if (sb_puts_json_escaped(&s, opts->msgs[i].role) < 0) goto fail;
		if (sb_puts(&s, "\",\"content\":\"") < 0) goto fail;
		if (sb_puts_json_escaped(&s, opts->msgs[i].content) < 0) goto fail;
		if (sb_puts(&s, "\"}") < 0) goto fail;
	}
	if (sb_puts(&s, "]") < 0) goto fail;
	if (opts->max_tokens > 0) {
		char buf[48];
		snprintf(buf, sizeof buf, ",\"max_tokens\":%d", opts->max_tokens);
		if (sb_puts(&s, buf) < 0) goto fail;
	}
	if (sb_putc(&s, '}') < 0) goto fail;
	return s.buf;
fail:
	free(s.buf);
	return NULL;
}

/* --- SSE streaming --- */

struct stream_ctx {
	const struct kora_client_chat_opts *opts;
	struct sb pending;        /* one SSE line across curl chunks */
	struct sb full_response;  /* accumulated assistant text */
	int done;
};

static void on_sse_line(struct stream_ctx *ctx, const char *line, size_t len)
{
	if (len < 6 || memcmp(line, "data: ", 6) != 0) return;
	const char *data = line + 6;
	size_t dlen = len - 6;
	if (dlen == 6 && memcmp(data, "[DONE]", 6) == 0) {
		ctx->done = 1;
		return;
	}
	/* make a null-terminated copy for strstr safety */
	char *copy = malloc(dlen + 1);
	if (!copy) return;
	memcpy(copy, data, dlen);
	copy[dlen] = '\0';

	/* unescape can only shrink, so dlen+1 is always enough. */
	char *token = malloc(dlen + 1);
	if (!token) { free(copy); return; }

	int n = kora_json_extract_content(copy, token, (int)(dlen + 1));
	if (n > 0) {
		if (ctx->opts->chunk_cb)
			ctx->opts->chunk_cb(token, n, ctx->opts->chunk_user_data);
		sb_append(&ctx->full_response, token, (size_t)n);
	}
	free(token);
	free(copy);
}

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	struct stream_ctx *ctx = userdata;
	size_t total = size * nmemb;

	if (ctx->opts->cancel && *ctx->opts->cancel) return 0;  /* signals abort */

	for (size_t i = 0; i < total; i++) {
		if (ptr[i] == '\n') {
			if (ctx->pending.buf)
				on_sse_line(ctx, ctx->pending.buf, ctx->pending.len);
			ctx->pending.len = 0;
			if (ctx->pending.buf) ctx->pending.buf[0] = '\0';
		} else if (ptr[i] != '\r') {
			sb_putc(&ctx->pending, ptr[i]);
		}
	}
	return total;
}

/* --- public: kora_client_chat --- */

int kora_client_chat(const struct kora_client_chat_opts *opts, char **out)
{
	if (out) *out = NULL;
	if (!opts || !opts->base_url || !opts->model) return -1;

	char url[512];
	snprintf(url, sizeof url, "%s/v1/chat/completions", opts->base_url);

	char *body = build_chat_body(opts);
	if (!body) return -1;

	struct stream_ctx ctx = {0};
	ctx.opts = opts;

	CURL *curl = curl_easy_init();
	if (!curl) { free(body); return -1; }

	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, "Accept: text/event-stream");

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

	CURLcode rc = curl_easy_perform(curl);

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	free(body);
	free(ctx.pending.buf);

	/* CURLE_WRITE_ERROR means write_cb returned 0, i.e. we cancelled. that's
	   a success from our perspective — partial response was captured. */
	if (rc != CURLE_OK && rc != CURLE_WRITE_ERROR) {
		free(ctx.full_response.buf);
		return -1;
	}

	if (out) *out = ctx.full_response.buf;
	else free(ctx.full_response.buf);
	return 0;
}

/* --- /tokenize --- */

struct collect_ctx { struct sb sb; };
static size_t collect_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
	struct collect_ctx *c = ud;
	size_t total = size * nmemb;
	sb_append(&c->sb, ptr, total);
	return total;
}

int kora_client_count_tokens(const char *base_url, const char *model, const char *text)
{
	if (!base_url || !text) return -1;
	char url[512];
	snprintf(url, sizeof url, "%s/tokenize", base_url);

	/* body: {"model":"...","content":"..."} */
	struct sb body = {0};
	sb_puts(&body, "{\"model\":\"");
	sb_puts_json_escaped(&body, model ? model : "");
	sb_puts(&body, "\",\"content\":\"");
	sb_puts_json_escaped(&body, text);
	sb_puts(&body, "\"}");

	CURL *curl = curl_easy_init();
	if (!curl) { free(body.buf); return -1; }

	struct collect_ctx collect = {0};
	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Content-Type: application/json");

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.buf);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &collect);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

	CURLcode rc = curl_easy_perform(curl);
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	free(body.buf);

	if (rc != CURLE_OK || status != 200 || !collect.sb.buf) {
		free(collect.sb.buf);
		return -1;
	}

	/* count array elements in "tokens":[...].
	   empty array → 0; otherwise commas + 1. */
	const char *arr = strstr(collect.sb.buf, "\"tokens\"");
	if (!arr) { free(collect.sb.buf); return -1; }
	arr = strchr(arr, '[');
	if (!arr) { free(collect.sb.buf); return -1; }
	arr++;
	/* skip whitespace */
	while (*arr == ' ' || *arr == '\n' || *arr == '\t') arr++;
	if (*arr == ']') { free(collect.sb.buf); return 0; }
	int count = 1;
	int depth = 1;
	for (const char *p = arr; *p && depth > 0; p++) {
		if (*p == '[') depth++;
		else if (*p == ']') { depth--; if (depth == 0) break; }
		else if (*p == ',' && depth == 1) count++;
	}
	free(collect.sb.buf);
	return count;
}

/* --- /kora/status ping --- */

int kora_client_ping(const char *base_url)
{
	if (!base_url) return -1;
	char url[512];
	snprintf(url, sizeof url, "%s/kora/status", base_url);

	CURL *curl = curl_easy_init();
	if (!curl) return -1;

	struct collect_ctx collect = {0};
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &collect);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

	CURLcode rc = curl_easy_perform(curl);
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_cleanup(curl);
	free(collect.sb.buf);

	return (rc == CURLE_OK && status == 200) ? 0 : -1;
}
