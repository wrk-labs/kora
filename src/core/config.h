#ifndef KORA_CONFIG_H
#define KORA_CONFIG_H

struct kora_config {
	char *default_model;
	char *chat_model;
	char *system_chat;
	char *compact_chat;
	int ctx_size;
	int threads;
	int gpu_layers;        /* -2 = auto, -1 = all, 0 = cpu, N = fixed */
};

struct kora_config *kora_config_load(const char *luadir);
void kora_config_free(struct kora_config *cfg);

#endif
