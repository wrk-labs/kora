#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guards.h"
#include "lua_bridge.h"
#include "run.h"
#include "tui.h"

/* ===== per-run state =====
   stashed on r->guard_state. allocated lazily on first guard call,
   freed by kora_guards_state_free at the end of the run. */

#define GS_MAX_ALWAYS_ALLOW 16
#define GS_REPEAT_LIMIT     3
#define GS_FAILURE_LIMIT    3

struct guard_state {
	/* repeat detection: same (name|args) signature N times in a row */
	char *prev_call_sig;
	int repeat_count;

	/* failure streak: consecutive ok=false results */
	int failure_streak;

	/* user-approved tools for the rest of this run (set "always" answer) */
	char *always_allow[GS_MAX_ALWAYS_ALLOW];
	int n_always_allow;
};

static struct guard_state *gs_get(struct kora_run *r)
{
	if (!r)
		return NULL;
	if (!r->guard_state)
		r->guard_state = calloc(1, sizeof(struct guard_state));
	return (struct guard_state *)r->guard_state;
}

void kora_guards_reset_per_call(struct kora_run *r)
{
	if (!r || !r->guard_state)
		return;
	struct guard_state *gs = r->guard_state;
	free(gs->prev_call_sig);
	gs->prev_call_sig = NULL;
	gs->repeat_count = 0;
	gs->failure_streak = 0;
	/* always_allow[] is preserved across calls within the same run */
}

void kora_guards_state_free(struct kora_run *r)
{
	if (!r || !r->guard_state)
		return;
	struct guard_state *gs = r->guard_state;
	free(gs->prev_call_sig);
	int i;
	for (i = 0; i < gs->n_always_allow; i++)
		free(gs->always_allow[i]);
	free(gs);
	r->guard_state = NULL;
}

/* ===== built-in guards ===== */

static struct kora_guard_result allow(void)
{
	struct kora_guard_result r = { KORA_VERDICT_ALLOW, NULL };
	return r;
}

static struct kora_guard_result deny(const char *reason)
{
	struct kora_guard_result r = { KORA_VERDICT_DENY, strdup(reason) };
	return r;
}

static struct kora_guard_result abort_v(const char *reason)
{
	struct kora_guard_result r = { KORA_VERDICT_ABORT, strdup(reason) };
	return r;
}

/* --- 1. step limit --- */
static struct kora_guard_result guard_step_limit(const struct kora_call *c)
{
	if (c->run->steps >= KORA_MAX_STEPS - 1)
		return abort_v("step limit exceeded");
	return allow();
}

/* --- 2. unknown name (defense in depth; Lua already checks too) --- */
static struct kora_guard_result guard_unknown_name(const struct kora_call *c)
{
	if (!c->name || !*c->name) {
		/* salvage path will try; let it through */
		return allow();
	}
	return allow();
}

/* --- 3. repeat-call detector --- */
static struct kora_guard_result guard_repeat(const struct kora_call *c)
{
	struct guard_state *gs = gs_get(c->run);
	if (!gs)
		return allow();

	char *sig = NULL;
	asprintf(&sig, "%s|%s", c->name ? c->name : "",
		c->args_json ? c->args_json : "");
	if (!sig)
		return allow();

	if (gs->prev_call_sig && strcmp(gs->prev_call_sig, sig) == 0) {
		gs->repeat_count++;
	} else {
		gs->repeat_count = 1;
		free(gs->prev_call_sig);
		gs->prev_call_sig = strdup(sig);
	}
	free(sig);

	if (gs->repeat_count >= GS_REPEAT_LIMIT)
		return abort_v("model is repeating the same tool call");
	return allow();
}

/* --- 4. failure streak --- */
static struct kora_guard_result guard_failure_streak(const struct kora_call *c)
{
	struct guard_state *gs = gs_get(c->run);
	if (!gs)
		return allow();
	if (gs->failure_streak >= GS_FAILURE_LIMIT)
		return abort_v("too many consecutive tool failures");
	return allow();
}

/* --- 5. permission policy ---
   ask the user based on the current agent's safety mode.
   - paranoid: ask for every tool
   - safe:     ask only for tools marked dangerous=true
   - unsafe:   never ask
   user answers cache in always_allow[]. */
static struct kora_guard_result guard_permission(const struct kora_call *c)
{
	struct kora_run *r = c->run;
	struct guard_state *gs = gs_get(r);
	if (!gs)
		return deny("no guard state");

	/* empty / missing tool name is never allowed silently — that means
	   parsing failed and we have no idea what's about to run. */
	if (!c->name || !*c->name)
		return deny("tool call has no name");

	const char *safety = kora_lua_agent_safety(r->agent_name);
	if (!safety) safety = "paranoid";  /* secure default */

	if (strcmp(safety, "unsafe") == 0)
		return allow();

	int needs_ask;
	if (strcmp(safety, "paranoid") == 0) {
		needs_ask = 1;
	} else {  /* "safe" */
		needs_ask = kora_lua_tool_is_dangerous(c->name);
	}
	if (!needs_ask)
		return allow();

	/* always-allow cache: walk up to the root run so a sub-agent inherits
	   the user's prior approvals from the parent. without this each sub-agent
	   would re-prompt for tools the user already greenlit. */
	struct kora_run *root = r;
	while (root->parent) root = root->parent;
	struct guard_state *root_gs = gs_get(root);
	if (root_gs) {
		int i;
		for (i = 0; i < root_gs->n_always_allow; i++) {
			if (strcmp(root_gs->always_allow[i], c->name) == 0)
				return allow();
		}
	}

	/* render the prompt: include the sub-agent name if we're not the root,
	   so the user knows which agent is asking */
	char prompt[256];
	if (r->parent) {
		snprintf(prompt, sizeof(prompt),
			"Run %s? (sub-agent: %s) [y]/[n]/[a]/[q]",
			c->name, r->agent_name ? r->agent_name : "?");
	} else {
		snprintf(prompt, sizeof(prompt),
			"Run %s? [y]es / [n]o / [a]lways / [q]uit", c->name);
	}
	/* the agent loop runs on a background thread; tui_request_permission
	   posts a cross-thread request that the main thread services, then
	   blocks here until the user answers. */
	int ans = tui_request_permission(prompt);

	if (ans == 'a') {
		/* persist the always-allow on the ROOT run so sub-agents see it too */
		struct guard_state *target = root_gs ? root_gs : gs;
		if (target->n_always_allow < GS_MAX_ALWAYS_ALLOW)
			target->always_allow[target->n_always_allow++] = strdup(c->name);
		return allow();
	}
	if (ans == 'y')
		return allow();
	if (ans == 'q')
		return abort_v("user aborted run");
	return deny("user denied this tool call");
}

/* ===== pipeline ===== */

static const kora_guard_fn pipeline[] = {
	guard_step_limit,
	guard_unknown_name,
	guard_repeat,
	guard_failure_streak,
	guard_permission,
	NULL,
};

struct kora_guard_result kora_guards_check(const struct kora_call *c)
{
	int i;
	for (i = 0; pipeline[i]; i++) {
		struct kora_guard_result v = pipeline[i](c);
		if (v.verdict != KORA_VERDICT_ALLOW)
			return v;
		free(v.reason);
	}
	return allow();
}

void kora_guards_record_result(struct kora_run *r, const char *result_json)
{
	struct guard_state *gs = gs_get(r);
	if (!gs)
		return;
	if (result_json && strstr(result_json, "\"ok\":false"))
		gs->failure_streak++;
	else
		gs->failure_streak = 0;
}
