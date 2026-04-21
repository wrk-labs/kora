#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "session.h"
#include "client.h"

static int ensure_cap(struct kora_session *s, int n)
{
	if (n <= s->cap_msg) return 0;
	int nc = s->cap_msg ? s->cap_msg * 2 : 16;
	while (nc < n) nc *= 2;
	char **nr = realloc(s->roles,    (size_t)nc * sizeof *s->roles);
	if (!nr) return -1;
	s->roles = nr;
	char **nc_ = realloc(s->contents, (size_t)nc * sizeof *s->contents);
	if (!nc_) return -1;
	s->contents = nc_;
	s->cap_msg  = nc;
	return 0;
}

int kora_session_add(struct kora_session *s, const char *role, const char *content)
{
	if (!s || !role || !content) return -1;
	if (ensure_cap(s, s->n_msg + 1) < 0) return -1;
	char *r = strdup(role);
	char *c = strdup(content);
	if (!r || !c) { free(r); free(c); return -1; }
	s->roles[s->n_msg]    = r;
	s->contents[s->n_msg] = c;
	s->n_msg++;
	return 0;
}

struct kora_session *kora_session_new(const char *model, const char *system_prompt)
{
	struct kora_session *s = calloc(1, sizeof *s);
	if (!s) return NULL;
	s->db_id = -1;
	if (model) {
		s->model = strdup(model);
		if (!s->model) goto fail;
	}
	if (system_prompt) {
		s->system_prompt = strdup(system_prompt);
		if (!s->system_prompt) goto fail;
		if (kora_session_add(s, "system", system_prompt) != 0) goto fail;
	}
	return s;
fail:
	kora_session_free(s);
	return NULL;
}

void kora_session_clear(struct kora_session *s)
{
	if (!s) return;
	int keep = (s->n_msg > 0 && strcmp(s->roles[0], "system") == 0) ? 1 : 0;
	for (int i = keep; i < s->n_msg; i++) {
		free(s->roles[i]);
		free(s->contents[i]);
	}
	s->n_msg = keep;
}

int kora_session_set_model(struct kora_session *s, const char *model)
{
	if (!s) return -1;
	char *dup = NULL;
	if (model && *model) {
		dup = strdup(model);
		if (!dup) return -1;
	}
	free(s->model);
	s->model = dup;
	return 0;
}

void kora_session_free(struct kora_session *s)
{
	if (!s) return;
	for (int i = 0; i < s->n_msg; i++) {
		free(s->roles[i]);
		free(s->contents[i]);
	}
	free(s->roles);
	free(s->contents);
	free(s->model);
	free(s->system_prompt);
	free(s);
}

int kora_session_approx_tokens(const struct kora_session *s)
{
	if (!s) return 0;
	size_t bytes = 0;
	for (int i = 0; i < s->n_msg; i++)
		bytes += strlen(s->roles[i]) + strlen(s->contents[i]) + 8;
	return (int)(bytes / 4);
}

int kora_session_snapshot(const struct kora_session *s, struct kora_message **out)
{
	if (!s || !out) return -1;
	*out = NULL;
	if (s->n_msg == 0) return 0;

	struct kora_message *msgs = calloc((size_t)s->n_msg, sizeof *msgs);
	if (!msgs) return -1;
	for (int i = 0; i < s->n_msg; i++) {
		char *r = strdup(s->roles[i]);
		char *c = strdup(s->contents[i]);
		if (!r || !c) {
			free(r); free(c);
			for (int j = 0; j < i; j++) {
				free((char *)msgs[j].role);
				free((char *)msgs[j].content);
			}
			free(msgs);
			return -1;
		}
		msgs[i].role    = r;
		msgs[i].content = c;
	}
	*out = msgs;
	return s->n_msg;
}

void kora_session_snapshot_free(struct kora_message *msgs, int n)
{
	if (!msgs) return;
	for (int i = 0; i < n; i++) {
		free((char *)msgs[i].role);
		free((char *)msgs[i].content);
	}
	free(msgs);
}

char *kora_session_transcript(const struct kora_session *s)
{
	if (!s) return NULL;
	size_t cap = 8192;
	size_t pos = 0;
	char *buf = malloc(cap);
	if (!buf) return NULL;
	buf[0] = '\0';

	int start = (s->n_msg > 0 && strcmp(s->roles[0], "system") == 0) ? 1 : 0;
	for (int i = start; i < s->n_msg; i++) {
		size_t need = strlen(s->roles[i]) + strlen(s->contents[i]) + 16;
		if (pos + need >= cap) {
			size_t nc = cap;
			while (nc < pos + need + 1) nc *= 2;
			char *nb = realloc(buf, nc);
			if (!nb) { free(buf); return NULL; }
			buf = nb;
			cap = nc;
		}
		pos += (size_t)snprintf(buf + pos, cap - pos,
		                        "%s: %s\n", s->roles[i], s->contents[i]);
	}
	return buf;
}
