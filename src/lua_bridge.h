#ifndef KORA_LUA_BRIDGE_H
#define KORA_LUA_BRIDGE_H

struct kora_config {
	char *default_model;
	char *chat_model;
	char *code_model;
	char *system_chat;
	char *system_code;
	int ctx_size;
	int threads;
	int gpu_layers;        /* -2 = auto, -1 = all, 0 = cpu, N = fixed */
};

/* initialize lua VM and load config
   looks for system prompts in luadir/core/system.lua
   looks for user config in ~/.kora/config.lua */
struct kora_config *kora_config_load(const char *luadir);

/* free config */
void kora_config_free(struct kora_config *cfg);

#endif
