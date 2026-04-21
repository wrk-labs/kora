#ifndef KORA_SERVER_INTERNAL_H
#define KORA_SERVER_INTERNAL_H

#ifdef __cplusplus
extern "C" {
#endif

/* --- request bookkeeping (server.c) --- */

void kora_server_begin_request(void);
void kora_server_end_request(void);

/* --- child lifecycle (child.c) --- */

void kora_child_init(int ctx_size);
void kora_child_set_default_model(const char *model);

/* returns the internal port of a running child serving `requested` (or the
   configured default if requested is NULL/empty), spawning on miss. blocks
   during spawn + health check. thread-safe. returns -1 on failure. */
int kora_child_ensure_ready(const char *requested);

void kora_child_stop(void);
int  kora_child_is_running(void);

/* --- proxy (proxy.cc) --- */

int  kora_proxy_listen(int public_port);
void kora_proxy_stop(void);

#ifdef __cplusplus
}
#endif

#endif
