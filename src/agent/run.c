#include <stdlib.h>
#include <string.h>

#include "guards.h"
#include "inference.h"
#include "run.h"

struct kora_run *kora_run_new(struct kora_ctx *kc,
                              volatile int *cancel,
                              struct kora_run *parent)
{
	struct kora_run *r = calloc(1, sizeof(*r));
	if (!r)
		return NULL;
	r->kc = kc;
	r->cancel = cancel;
	r->parent = parent;
	r->depth = parent ? parent->depth + 1 : 0;
	r->show_in_tui = parent ? 0 : 1;
	/* prefer native tool calling when the model's chat template supports
	   it. otherwise fall back to the harness <tool_call> parser. */
	r->mode = (kc && kc->native_supports_tools) ? KORA_MODE_NATIVE : KORA_MODE_HARNESS;
	return r;
}

void kora_run_clear_msgs(struct kora_run *r)
{
	int i;
	for (i = 0; i < r->n_msgs; i++) {
		free(r->msgs[i].role);
		free(r->msgs[i].content);
		free(r->msgs[i].name);
	}
	r->n_msgs = 0;
}

void kora_run_free(struct kora_run *r)
{
	if (!r)
		return;
	kora_guards_state_free(r);
	kora_run_clear_msgs(r);
	free(r);
}

int kora_run_push_msg(struct kora_run *r,
                      const char *role,
                      const char *content,
                      const char *name)
{
	if (r->n_msgs >= KORA_MAX_MESSAGES)
		return -1;
	struct kora_message *m = &r->msgs[r->n_msgs++];
	m->role = role ? strdup(role) : NULL;
	m->content = content ? strdup(content) : NULL;
	m->name = name ? strdup(name) : NULL;
	return 0;
}
