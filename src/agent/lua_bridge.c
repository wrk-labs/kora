#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lua_bridge.h"
#include "run.h"
#include "util.h"

/* ===== config loader (existing, transient VM) ===== */

static char *lua_getfield_str(lua_State *L, int idx, const char *key)
{
	lua_getfield(L, idx, key);
	const char *s = lua_tostring(L, -1);
	char *dup = s ? strdup(s) : NULL;
	lua_pop(L, 1);
	return dup;
}

static int lua_getfield_int(lua_State *L, int idx, const char *key, int def)
{
	lua_getfield(L, idx, key);
	int val = lua_isinteger(L, -1) ? (int)lua_tointeger(L, -1) : def;
	lua_pop(L, 1);
	return val;
}

struct kora_config *kora_config_load(const char *luadir)
{
	struct kora_config *cfg = calloc(1, sizeof(*cfg));
	if (!cfg)
		return NULL;

	cfg->default_model = strdup("llama-3.2-3b");
	cfg->chat_model = NULL;
	cfg->code_model = NULL;
	cfg->system_chat = NULL;
	cfg->system_code = NULL;
	cfg->ctx_size = 4096;
	cfg->threads = 0;
	cfg->gpu_layers = -2;

	lua_State *L = luaL_newstate();
	if (!L)
		return cfg;
	luaL_openlibs(L);

	char syspath[512];
	snprintf(syspath, sizeof(syspath), "%s/core/system.lua", luadir);
	if (luaL_dofile(L, syspath) != LUA_OK) {
		lua_pop(L, 1);
		snprintf(syspath, sizeof(syspath), "lua/core/system.lua");
		(void)luaL_dofile(L, syspath);
	}
	if (lua_istable(L, -1)) {
		cfg->system_chat = lua_getfield_str(L, -1, "chat");
		cfg->system_code = lua_getfield_str(L, -1, "code");
		cfg->compact_chat = lua_getfield_str(L, -1, "compact_chat");
		cfg->compact_code = lua_getfield_str(L, -1, "compact_code");
		lua_pop(L, 1);
	}

	char *cfgpath = kora_path("config.lua");
	if (cfgpath && luaL_dofile(L, cfgpath) == LUA_OK) {
		char *s;
		s = lua_getfield_str(L, -1, "default_model");
		if (s) { free(cfg->default_model); cfg->default_model = s; }
		s = lua_getfield_str(L, -1, "chat_model");
		if (s) { free(cfg->chat_model); cfg->chat_model = s; }
		s = lua_getfield_str(L, -1, "code_model");
		if (s) { free(cfg->code_model); cfg->code_model = s; }
		cfg->ctx_size = lua_getfield_int(L, -1, "ctx_size", cfg->ctx_size);
		cfg->threads = lua_getfield_int(L, -1, "threads", cfg->threads);
		cfg->gpu_layers = lua_getfield_int(L, -1, "gpu_layers", cfg->gpu_layers);
		lua_pop(L, 1);
	}
	free(cfgpath);

	lua_close(L);
	return cfg;
}

void kora_config_free(struct kora_config *cfg)
{
	if (!cfg)
		return;
	free(cfg->default_model);
	free(cfg->chat_model);
	free(cfg->code_model);
	free(cfg->system_chat);
	free(cfg->system_code);
	free(cfg->compact_chat);
	free(cfg->compact_code);
	free(cfg);
}

/* ===== persistent agent runtime ===== */

static lua_State *RT = NULL;
static struct kora_run *current_run = NULL;

void kora_lua_set_current_run(struct kora_run *r) { current_run = r; }
struct kora_run *kora_lua_current_run(void) { return current_run; }

/* --- C functions exposed as kora.* --- */

/* kora.shell_exec(cmd, timeout_ms) -> (stdout_string, exit_code)
   forks, runs cmd via /bin/sh -c, enforces timeout by killing the process
   group, caps output at 256 KB. */
static int l_shell_exec(lua_State *L)
{
	const char *cmd = luaL_checkstring(L, 1);
	int timeout_ms = (int)luaL_optinteger(L, 2, 30000);

	int pipefd[2];
	if (pipe(pipefd) < 0) {
		lua_pushnil(L);
		lua_pushinteger(L, -1);
		return 2;
	}

	pid_t pid = fork();
	if (pid < 0) {
		close(pipefd[0]); close(pipefd[1]);
		lua_pushnil(L);
		lua_pushinteger(L, -1);
		return 2;
	}

	if (pid == 0) {
		/* child */
		setpgid(0, 0);
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}

	/* parent */
	close(pipefd[1]);
	setpgid(pid, pid);

	const size_t cap = 256 * 1024;
	char *buf = malloc(cap);
	size_t len = 0;
	int killed = 0;

	struct timeval start, now;
	gettimeofday(&start, NULL);

	int flags = fcntl(pipefd[0], F_GETFL, 0);
	fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

	while (1) {
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(pipefd[0], &rfds);

		gettimeofday(&now, NULL);
		long elapsed = (now.tv_sec - start.tv_sec) * 1000 +
		               (now.tv_usec - start.tv_usec) / 1000;
		long remaining = timeout_ms - elapsed;
		if (remaining <= 0) {
			killpg(pid, SIGKILL);
			killed = 1;
			break;
		}

		struct timeval tv = { remaining / 1000, (remaining % 1000) * 1000 };
		int rv = select(pipefd[0] + 1, &rfds, NULL, NULL, &tv);
		if (rv < 0) {
			if (errno == EINTR) continue;
			break;
		}
		if (rv == 0) {
			killpg(pid, SIGKILL);
			killed = 1;
			break;
		}

		ssize_t n = read(pipefd[0], buf + len, cap - len - 1);
		if (n <= 0) {
			if (n < 0 && errno == EAGAIN) continue;
			break;  /* EOF */
		}
		len += n;
		if (len >= cap - 1) {
			killpg(pid, SIGKILL);
			killed = 1;
			break;
		}
	}

	close(pipefd[0]);

	int status = 0;
	waitpid(pid, &status, 0);

	buf[len] = '\0';
	int exit_code = killed ? -1 :
	                (WIFEXITED(status) ? WEXITSTATUS(status) : -1);

	lua_pushlstring(L, buf, len);
	lua_pushinteger(L, exit_code);
	free(buf);
	return 2;
}

/* kora.todos_set(list) — store the todo list on the current run.
   for now we stash it as a Lua table inside the registry; persisting to
   SQLite happens later when we extend the sessions schema. */
static int l_todos_set(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_setfield(L, LUA_REGISTRYINDEX, "kora.todos");
	return 0;
}

static int l_todos_get(lua_State *L)
{
	lua_getfield(L, LUA_REGISTRYINDEX, "kora.todos");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		lua_newtable(L);
	}
	return 1;
}

/* kora.event_push(kind, payload) — best-effort UI event hook.
   for phase 1 we just write to stderr; the TUI integration lands in
   the loop wiring step. */
static int l_event_push(lua_State *L)
{
	const char *kind = luaL_optstring(L, 1, "info");
	const char *payload = luaL_optstring(L, 2, "");
	fprintf(stderr, "[kora.%s] %s\n", kind, payload);
	return 0;
}

/* kora.loop_run(agent_name, prompt) -> result | nil, err
   spawns a child run for a sub-agent. delegates to the C loop, which is
   defined in src/agent/loop.c — declared here as an extern to avoid a
   header dependency cycle. */
extern char *kora_loop_run_subagent(struct kora_run *parent,
                                    const char *agent_name,
                                    const char *prompt,
                                    char **err_out);

static int l_loop_run(lua_State *L)
{
	const char *agent = luaL_checkstring(L, 1);
	const char *prompt = luaL_checkstring(L, 2);

	struct kora_run *parent = current_run;
	if (!parent) {
		lua_pushnil(L);
		lua_pushstring(L, "no current run");
		return 2;
	}

	char *err = NULL;
	char *result = kora_loop_run_subagent(parent, agent, prompt, &err);
	if (!result) {
		lua_pushnil(L);
		lua_pushstring(L, err ? err : "sub-agent failed");
		free(err);
		return 2;
	}
	lua_pushstring(L, result);
	free(result);
	return 1;
}

/* kora.confirm(prompt) -> "y" | "n" | "a"
   stub for phase 1; always returns "y". permission gate lands in phase 2. */
static int l_confirm(lua_State *L)
{
	(void)L;
	lua_pushstring(L, "y");
	return 1;
}

/* --- runtime init/teardown --- */

static void install_kora_table(lua_State *L)
{
	lua_newtable(L);
	lua_pushcfunction(L, l_shell_exec); lua_setfield(L, -2, "shell_exec");
	lua_pushcfunction(L, l_todos_set);  lua_setfield(L, -2, "todos_set");
	lua_pushcfunction(L, l_todos_get);  lua_setfield(L, -2, "todos_get");
	lua_pushcfunction(L, l_event_push); lua_setfield(L, -2, "event_push");
	lua_pushcfunction(L, l_loop_run);   lua_setfield(L, -2, "loop_run");
	lua_pushcfunction(L, l_confirm);    lua_setfield(L, -2, "confirm");
	lua_setglobal(L, "kora");
}

/* check whether <dir>/core/loader.lua exists */
static int has_loader(const char *dir)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/core/loader.lua", dir);
	return access(path, R_OK) == 0;
}

int kora_lua_runtime_init(const char *luadir)
{
	if (RT)
		return 0;

	/* prefer the installed path; fall back to ./lua for in-tree dev runs */
	const char *resolved = NULL;
	if (luadir && has_loader(luadir))
		resolved = luadir;
	else if (has_loader("lua"))
		resolved = "lua";
	else if (has_loader("./lua"))
		resolved = "./lua";

	if (!resolved) {
		fprintf(stderr,
			"kora: cannot locate lua runtime (tried %s and ./lua). "
			"Run 'make install' or invoke from the kora source directory.\n",
			luadir ? luadir : "(null)");
		return -1;
	}

	RT = luaL_newstate();
	if (!RT)
		return -1;
	luaL_openlibs(RT);

	/* set package.path so require('core.loader'), require('tools.read'), etc work */
	lua_getglobal(RT, "package");
	lua_pushfstring(RT, "%s/?.lua;%s/?/init.lua", resolved, resolved);
	lua_setfield(RT, -2, "path");
	lua_pop(RT, 1);

	install_kora_table(RT);

	/* load loader.lua and call M.bootstrap(resolved) */
	char path[512];
	snprintf(path, sizeof(path), "%s/core/loader.lua", resolved);
	if (luaL_dofile(RT, path) != LUA_OK) {
		fprintf(stderr, "kora: failed to load loader.lua: %s\n",
			lua_tostring(RT, -1));
		lua_close(RT);
		RT = NULL;
		return -1;
	}
	/* loader returns its module table; call M.bootstrap(luadir) */
	if (!lua_istable(RT, -1)) {
		fprintf(stderr, "kora: loader.lua did not return a table\n");
		lua_close(RT);
		RT = NULL;
		return -1;
	}
	lua_getfield(RT, -1, "bootstrap");
	lua_pushstring(RT, resolved);
	if (lua_pcall(RT, 1, 0, 0) != LUA_OK) {
		fprintf(stderr, "kora: bootstrap failed: %s\n", lua_tostring(RT, -1));
		lua_close(RT);
		RT = NULL;
		return -1;
	}
	lua_pop(RT, 1);  /* loader module table */
	return 0;
}

void kora_lua_runtime_free(void)
{
	if (!RT)
		return;
	lua_close(RT);
	RT = NULL;
}

char *kora_lua_dispatch_tool(const char *name, const char *args_json)
{
	if (!RT)
		return NULL;
	lua_getglobal(RT, "kora");
	lua_getfield(RT, -1, "dispatch_tool");
	lua_pushstring(RT, name);
	lua_pushstring(RT, args_json ? args_json : "{}");
	if (lua_pcall(RT, 2, 1, 0) != LUA_OK) {
		const char *err = lua_tostring(RT, -1);
		char *result;
		asprintf(&result, "{\"ok\":false,\"error\":\"lua dispatch failed: %s\"}",
			err ? err : "unknown");
		lua_pop(RT, 2);
		return result;
	}
	const char *s = lua_tostring(RT, -1);
	char *result = s ? strdup(s) : NULL;
	lua_pop(RT, 2);  /* result + kora */
	return result;
}

char *kora_lua_build_system_prompt(const char *agent_name, const char *mode)
{
	if (!RT)
		return NULL;
	lua_getglobal(RT, "kora");
	lua_getfield(RT, -1, "build_system_prompt");
	lua_pushstring(RT, agent_name);
	lua_pushstring(RT, mode ? mode : "native");
	if (lua_pcall(RT, 2, 1, 0) != LUA_OK) {
		fprintf(stderr, "kora: build_system_prompt failed: %s\n",
			lua_tostring(RT, -1));
		lua_pop(RT, 2);
		return NULL;
	}
	const char *s = lua_tostring(RT, -1);
	char *result = s ? strdup(s) : NULL;
	lua_pop(RT, 2);
	return result;
}

int kora_lua_agent_exists(const char *name, const char *parent_name)
{
	if (!RT)
		return 0;
	lua_getglobal(RT, "kora");
	lua_getfield(RT, -1, "resolve_agent");
	lua_pushstring(RT, name);
	if (parent_name)
		lua_pushstring(RT, parent_name);
	else
		lua_pushnil(RT);
	if (lua_pcall(RT, 2, 2, 0) != LUA_OK) {
		lua_pop(RT, 2);
		return 0;
	}
	int ok = !lua_isnil(RT, -2);
	lua_pop(RT, 3);  /* 2 results + kora */
	return ok;
}

int kora_lua_set_run_tool_whitelist(const char *agent_name)
{
	if (!RT)
		return -1;
	lua_getglobal(RT, "kora");
	lua_getfield(RT, -1, "agents");
	lua_getfield(RT, -1, agent_name);
	if (!lua_istable(RT, -1)) {
		lua_pop(RT, 3);
		return -1;
	}
	lua_getfield(RT, -1, "_tool_set");
	/* set kora._current_tools = the set */
	lua_setfield(RT, -4, "_current_tools");
	lua_pop(RT, 3);
	return 0;
}

void kora_lua_clear_run_tool_whitelist(void)
{
	if (!RT)
		return;
	lua_getglobal(RT, "kora");
	lua_pushnil(RT);
	lua_setfield(RT, -2, "_current_tools");
	lua_pop(RT, 1);
}

int kora_lua_tool_is_dangerous(const char *name)
{
	if (!RT || !name)
		return 0;
	lua_getglobal(RT, "kora");
	lua_getfield(RT, -1, "tools");
	lua_getfield(RT, -1, name);
	if (!lua_istable(RT, -1)) {
		lua_pop(RT, 3);
		return 0;
	}
	lua_getfield(RT, -1, "dangerous");
	int dangerous = lua_toboolean(RT, -1);
	lua_pop(RT, 4);
	return dangerous;
}

int kora_lua_collect_tools(const char *agent_name,
                           char ***out_names,
                           char ***out_descs,
                           char ***out_schemas)
{
	*out_names = NULL;
	*out_descs = NULL;
	*out_schemas = NULL;
	if (!RT || !agent_name)
		return -1;

	/* fetch the agent table → its `tools` array */
	lua_getglobal(RT, "kora");
	lua_getfield(RT, -1, "agents");
	lua_getfield(RT, -1, agent_name);
	if (!lua_istable(RT, -1)) {
		lua_pop(RT, 3);
		return -1;
	}
	lua_getfield(RT, -1, "tools");
	if (!lua_istable(RT, -1)) {
		lua_pop(RT, 4);
		return 0;
	}
	int n = (int)lua_rawlen(RT, -1);
	if (n == 0) {
		lua_pop(RT, 4);
		return 0;
	}

	char **names = calloc(n, sizeof(char *));
	char **descs = calloc(n, sizeof(char *));
	char **schemas = calloc(n, sizeof(char *));

	int i;
	for (i = 0; i < n; i++) {
		lua_rawgeti(RT, -1, i + 1);  /* tool name */
		const char *tname = lua_tostring(RT, -1);
		if (!tname) {
			lua_pop(RT, 1);
			continue;
		}
		names[i] = strdup(tname);

		/* look up kora.tools[tname] */
		lua_getglobal(RT, "kora");
		lua_getfield(RT, -1, "tools");
		lua_getfield(RT, -1, tname);
		if (lua_istable(RT, -1)) {
			lua_getfield(RT, -1, "description");
			const char *d = lua_tostring(RT, -1);
			descs[i] = strdup(d ? d : "");
			lua_pop(RT, 1);

			/* call kora.tool_json_schema(tool) to get the schema string */
			lua_getglobal(RT, "kora");
			lua_getfield(RT, -1, "tool_json_schema");
			lua_pushvalue(RT, -3);  /* the tool table */
			if (lua_pcall(RT, 1, 1, 0) == LUA_OK) {
				const char *s = lua_tostring(RT, -1);
				schemas[i] = strdup(s ? s : "{\"type\":\"object\",\"properties\":{}}");
			} else {
				schemas[i] = strdup("{\"type\":\"object\",\"properties\":{}}");
			}
			lua_pop(RT, 2);  /* schema result + kora */
		} else {
			descs[i] = strdup("");
			schemas[i] = strdup("{\"type\":\"object\",\"properties\":{}}");
		}
		lua_pop(RT, 4);  /* tool? + tools + kora + tool_name string */
	}

	lua_pop(RT, 4);  /* agent.tools + agent + agents + kora */

	*out_names = names;
	*out_descs = descs;
	*out_schemas = schemas;
	return n;
}

void kora_lua_tools_free(char **names, char **descs, char **schemas, int n)
{
	int i;
	for (i = 0; i < n; i++) {
		free(names[i]);
		free(descs[i]);
		free(schemas[i]);
	}
	free(names);
	free(descs);
	free(schemas);
}

int kora_lua_warm_index(void)
{
	if (!RT)
		return -1;
	/* require('core.index') */
	lua_getglobal(RT, "require");
	lua_pushstring(RT, "core.index");
	if (lua_pcall(RT, 1, 1, 0) != LUA_OK) {
		fprintf(stderr, "kora: failed to require core.index: %s\n",
			lua_tostring(RT, -1));
		lua_pop(RT, 1);
		return -1;
	}
	if (!lua_istable(RT, -1)) {
		lua_pop(RT, 1);
		return -1;
	}
	lua_getfield(RT, -1, "scan");

	/* arg: pwd from C (so the Lua side doesn't shell-out unnecessarily) */
	char cwd[1024];
	if (!getcwd(cwd, sizeof(cwd))) {
		lua_pop(RT, 2);
		return -1;
	}
	lua_pushstring(RT, cwd);

	if (lua_pcall(RT, 1, 0, 0) != LUA_OK) {
		fprintf(stderr, "kora: index scan failed: %s\n", lua_tostring(RT, -1));
		lua_pop(RT, 2);
		return -1;
	}
	lua_pop(RT, 1);  /* core.index module table */
	return 0;
}

const char *kora_lua_agent_safety(const char *agent_name)
{
	if (!RT || !agent_name)
		return NULL;
	lua_getglobal(RT, "kora");
	lua_getfield(RT, -1, "agents");
	lua_getfield(RT, -1, agent_name);
	if (!lua_istable(RT, -1)) {
		lua_pop(RT, 3);
		return NULL;
	}
	lua_getfield(RT, -1, "safety");
	const char *s = lua_tostring(RT, -1);
	/* normalize to one of three string-literal pointers so the caller
	   can hold the pointer past the lua_pop */
	const char *result = NULL;
	if (s) {
		if (strcmp(s, "paranoid") == 0) result = "paranoid";
		else if (strcmp(s, "safe") == 0) result = "safe";
		else if (strcmp(s, "unsafe") == 0) result = "unsafe";
	}
	lua_pop(RT, 4);
	return result;
}
