#ifndef KORA_LUA_BRIDGE_H
#define KORA_LUA_BRIDGE_H

struct kora_run;

struct kora_config {
	char *default_model;
	char *chat_model;
	char *code_model;
	char *system_chat;
	char *system_code;
	char *compact_chat;
	char *compact_code;
	int ctx_size;
	int threads;
	int gpu_layers;        /* -2 = auto, -1 = all, 0 = cpu, N = fixed */
};

/* initialize lua VM and load config (transient VM, used at startup only) */
struct kora_config *kora_config_load(const char *luadir);

/* free config */
void kora_config_free(struct kora_config *cfg);

/* ----- persistent agent runtime ----- */

/* initialize the persistent Lua VM that holds tools, agents, and registries.
   loads lua/core/loader.lua and runs the bootstrap which registers everything.
   returns 0 on success, -1 on failure. idempotent (returns 0 if already up). */
int kora_lua_runtime_init(const char *luadir);

/* shutdown and free the runtime VM */
void kora_lua_runtime_free(void);

/* set the "current run" pointer used by C bindings called from Lua
   (kora.loop_run, kora.todos_set, etc). single active run at a time. */
void kora_lua_set_current_run(struct kora_run *r);
struct kora_run *kora_lua_current_run(void);

/* dispatch a tool call: hands name+args_json to kora.dispatch_tool in Lua,
   returns the JSON-encoded result string. caller frees. */
char *kora_lua_dispatch_tool(const char *name, const char *args_json);

/* build the system prompt for an agent (calls kora.build_system_prompt).
   mode is "native" or "harness" — controls whether the harness format
   teaching is included in the prompt. returns allocated string, caller frees. */
char *kora_lua_build_system_prompt(const char *agent_name, const char *mode);

/* check if an agent exists and is reachable from `parent`
   (or top-level if parent is NULL). returns 1 if ok, 0 otherwise. */
int kora_lua_agent_exists(const char *name, const char *parent_name);

/* fetch the tool whitelist for an agent as a Lua table at the top of the
   stack — used internally by the dispatcher. returns 0 on success. */
int kora_lua_set_run_tool_whitelist(const char *agent_name);
void kora_lua_clear_run_tool_whitelist(void);

/* returns 1 if a registered tool has dangerous=true, 0 otherwise (or if
   the tool does not exist). */
int kora_lua_tool_is_dangerous(const char *name);

/* returns the agent's `safety` field as a static string ("paranoid",
   "safe", "unsafe"), or NULL if the agent or field is missing.
   the returned string is owned by Lua; do not free. */
const char *kora_lua_agent_safety(const char *agent_name);

/* warm the project index by calling core.index.scan(cwd). caches the
   result on the index module so the first agent prompt doesn't pay the
   scan cost. callable from a background thread BEFORE the agent thread
   starts (single-threaded Lua-VM access required). returns 0 on success. */
int kora_lua_warm_index(void);

/* collect the declared tools for an agent into parallel allocated arrays
   of strings (name, description, JSON Schema for parameters). caller frees
   each cell + the arrays via kora_lua_tools_free.
   returns the count, or -1 on error. */
int kora_lua_collect_tools(const char *agent_name,
                           char ***out_names,
                           char ***out_descs,
                           char ***out_schemas);
void kora_lua_tools_free(char **names, char **descs, char **schemas, int n);

#endif
