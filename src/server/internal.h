#ifndef KORA_SERVER_INTERNAL_H
#define KORA_SERVER_INTERNAL_H

#ifdef __cplusplus
extern "C" {
#endif

/* --- pool (pool.c) --- */

struct kora_pool_snap {
	char model[128];
	int  port;
	int  pid;
	int  in_flight;
	int  idle_secs;
	int  loading;
};

void kora_pool_init(int cap, int ctx_size, int parallel);
void kora_pool_set_default_model(const char *model);
const char *kora_pool_default_model(void);

/* ensures a child is running for `requested` (or the default if NULL/empty).
   on success: returns the internal port and writes the pool slot index into
   *out_slot. bumps in_flight; caller MUST call kora_pool_release(slot)
   exactly once when done. returns -1 on failure. */
int  kora_pool_ensure_ready(const char *requested, int *out_slot);

void kora_pool_release(int slot);

int  kora_pool_is_any_running(void);
void kora_pool_idle_check(int timeout_secs);
void kora_pool_stop_all(void);

/* admin: returns 0 on success, -1 if not loaded, -2 if busy */
int  kora_pool_unload(const char *model);

/* snapshot for admin/status */
int  kora_pool_snapshot(struct kora_pool_snap *out, int cap);
int  kora_pool_cap(void);
int  kora_pool_ctx_size(void);
int  kora_pool_parallel(void);

/* --- proxy (proxy.cc) --- */

int  kora_proxy_listen(int public_port);
void kora_proxy_stop(void);

#ifdef __cplusplus
}
#endif

#endif
