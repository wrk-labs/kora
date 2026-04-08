#ifndef KORA_GUARDS_H
#define KORA_GUARDS_H

struct kora_run;

/* the verdict pipeline returns one of these for every tool call */
enum kora_verdict {
	KORA_VERDICT_ALLOW = 0,  /* run the tool */
	KORA_VERDICT_DENY,       /* skip; feed the reason back to the model */
	KORA_VERDICT_ABORT,      /* break the loop entirely */
};

/* a tool call about to be dispatched. owned by the loop. */
struct kora_call {
	const char *name;
	const char *args_json;
	struct kora_run *run;
};

/* result of running the guard pipeline. caller must free reason. */
struct kora_guard_result {
	enum kora_verdict verdict;
	char *reason;   /* allocated; may be NULL */
};

/* a guard function. each built-in guard implements this. */
typedef struct kora_guard_result (*kora_guard_fn)(const struct kora_call *c);

/* run all guards in order. first non-ALLOW verdict wins.
   the permission guard may interactively ask the user (TUI). */
struct kora_guard_result kora_guards_check(const struct kora_call *c);

/* called by the loop after a tool actually ran, so guards that track
   per-run state (failure streak, etc) can update. */
void kora_guards_record_result(struct kora_run *r, const char *result_json);

/* reset only the per-call counters (repeat detector, failure streak).
   PRESERVES the always-allow list so the user isn't re-asked across
   user messages in the same session. call at the start of each
   kora_loop_run invocation. */
void kora_guards_reset_per_call(struct kora_run *r);

/* free all per-run guard state. called by kora_run_free, NOT by the
   per-call cleanup. safe to call with NULL state. */
void kora_guards_state_free(struct kora_run *r);

#endif
