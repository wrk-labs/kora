#ifndef KORA_LOOP_H
#define KORA_LOOP_H

struct kora_run;

/* run the agent loop on an existing run frame.
   builds prompts, streams inference, dispatches tool calls, repeats
   until the model emits plain text with no tool call (or limits hit).
   returns the final assistant message (caller frees) or NULL on error.
   if err_out is non-null, sets *err_out to a strdup'd error string. */
char *kora_loop_run(struct kora_run *r,
                    const char *user_prompt,
                    char **err_out);

/* spawn a sub-agent: validate the parent's whitelist, create a child run,
   call kora_loop_run, return the result. invoked by the task tool via
   kora.loop_run in lua_bridge. */
char *kora_loop_run_subagent(struct kora_run *parent,
                             const char *agent_name,
                             const char *prompt,
                             char **err_out);

#endif
