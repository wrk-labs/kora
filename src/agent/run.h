#ifndef KORA_RUN_H
#define KORA_RUN_H

struct kora_ctx;

/* recursion + step limits, mirrors opencode's stopWhen */
#define KORA_MAX_DEPTH 3
#define KORA_MAX_STEPS 100
#define KORA_MAX_MESSAGES 512

enum kora_run_mode {
	KORA_MODE_HARNESS = 0,  /* phase 1: <tool_call> markers in raw text */
	KORA_MODE_NATIVE  = 1,  /* phase 1.5: llama.cpp tool API */
};

struct kora_message {
	char *role;     /* "system" | "user" | "assistant" | "tool" */
	char *content;  /* message body */
	char *name;     /* optional tool name for tool-role messages */
};

/* a run is one invocation of the agent loop. it owns its own message
   array; sub-agents create child runs. ephemeral, never persisted. */
struct kora_run {
	struct kora_ctx *kc;            /* shared inference context */

	struct kora_message msgs[KORA_MAX_MESSAGES];
	int n_msgs;

	const char *agent_name;         /* current agent (Lua-owned string) */
	int depth;                      /* sub-agent recursion depth */
	int steps;                      /* tool-call iterations this run */

	enum kora_run_mode mode;
	int native_failures;            /* downgrade trigger for "auto" mode */

	volatile int *cancel;           /* shared cancel flag (ESC) */

	int show_in_tui;                /* 1 = main run, 0 = sub-agent */

	struct kora_run *parent;        /* NULL for top-level */

	void *guard_state;              /* opaque, owned by guards.c */
};

/* allocate and initialize a run frame.
   parent may be NULL for a top-level run.
   cancel is a shared flag pointer (children inherit the parent's). */
struct kora_run *kora_run_new(struct kora_ctx *kc,
                              volatile int *cancel,
                              struct kora_run *parent);

/* free a run and all owned messages */
void kora_run_free(struct kora_run *r);

/* append a message; copies role/content/name */
int kora_run_push_msg(struct kora_run *r,
                      const char *role,
                      const char *content,
                      const char *name);

/* clear all messages */
void kora_run_clear_msgs(struct kora_run *r);

#endif
