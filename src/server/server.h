#ifndef KORA_SERVER_H
#define KORA_SERVER_H

struct kora_server_opts {
	const char *model;              /* default model alias (required) */
	int         public_port;        /* 0 → 8818 */
	int         ctx_size;           /* 0 → 8192 */
	int         idle_timeout_secs;  /* 0 → never unload */
	int         pool_size;          /* 0 → 2 (max simultaneous resident models) */
	int         parallel;           /* 0 → 1 (per-model llama-server slots) */
};

/* run the supervisor. binds public_port, lazy-spawns llama-server as a
   child on first request, proxies OpenAI-compat HTTP, idle-unloads when
   no requests for idle_timeout_secs. returns on SIGINT/SIGTERM. */
int kora_server_run(const struct kora_server_opts *opts);

#endif
