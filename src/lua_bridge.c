#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lua_bridge.h"
#include "util.h"

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

	/* defaults */
	cfg->default_model = strdup("llama-3.2-3b");
	cfg->chat_model = NULL;
	cfg->code_model = NULL;
	cfg->system_chat = NULL;
	cfg->system_code = NULL;
	cfg->ctx_size = 4096;
	cfg->threads = 0;
	cfg->gpu_layers = -2; /* auto */

	lua_State *L = luaL_newstate();
	if (!L)
		return cfg;
	luaL_openlibs(L);

	/* load system prompts: try installed path, then local ./lua/ */
	char syspath[512];
	snprintf(syspath, sizeof(syspath), "%s/core/system.lua", luadir);
	if (luaL_dofile(L, syspath) != LUA_OK) {
		lua_pop(L, 1);
		snprintf(syspath, sizeof(syspath), "lua/core/system.lua");
		luaL_dofile(L, syspath);
	}
	if (lua_istable(L, -1)) {
		cfg->system_chat = lua_getfield_str(L, -1, "chat");
		cfg->system_code = lua_getfield_str(L, -1, "code");
		cfg->compact_chat = lua_getfield_str(L, -1, "compact_chat");
		cfg->compact_code = lua_getfield_str(L, -1, "compact_code");
		lua_pop(L, 1);
	}

	/* load user config from ~/.kora/config.lua */
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
