#define _GNU_SOURCE
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "internal.h"
#include "registry.h"
#include "util.h"

#ifndef KORA_POOL_MAX_RESIDENT
#define KORA_POOL_MAX_RESIDENT 8
#endif

struct pool_entry {
	pid_t pid;               /* 0 = slot free */
	int   port;              /* internal loopback port */
	char  model[128];        /* model alias */
	long  last_used_ns;      /* for LRU */
	long  last_request_ns;   /* for idle unload */
	int   in_flight;         /* active requests touching this entry */
	int   loading;           /* 1 while spawning llama-server */
};

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;

static struct pool_entry g_pool[KORA_POOL_MAX_RESIDENT];
static int  g_cap       = 2;
static int  g_ctx_size  = 8192;
static int  g_parallel  = 1;
static char g_default_model[128];

static long now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000000000L + ts.tv_nsec;
}

void kora_pool_init(int cap, int ctx_size, int parallel)
{
	if (cap >= 1 && cap <= KORA_POOL_MAX_RESIDENT) g_cap = cap;
	if (ctx_size > 0) g_ctx_size = ctx_size;
	if (parallel > 0) g_parallel = parallel;
}

void kora_pool_set_default_model(const char *m)
{
	snprintf(g_default_model, sizeof g_default_model, "%s", m ? m : "");
}

const char *kora_pool_default_model(void)
{
	return g_default_model;
}

/* caller holds g_mu */
static int find_loaded(const char *model)
{
	for (int i = 0; i < g_cap; i++)
		if (g_pool[i].pid > 0 && !g_pool[i].loading &&
		    strcmp(g_pool[i].model, model) == 0)
			return i;
	return -1;
}

static int find_loading(const char *model)
{
	for (int i = 0; i < g_cap; i++)
		if (g_pool[i].loading && strcmp(g_pool[i].model, model) == 0)
			return i;
	return -1;
}

static int find_empty(void)
{
	for (int i = 0; i < g_cap; i++)
		if (g_pool[i].pid == 0 && !g_pool[i].loading)
			return i;
	return -1;
}

static int find_lru_evictable(void)
{
	int best = -1;
	long best_ts = 0;
	for (int i = 0; i < g_cap; i++) {
		if (g_pool[i].pid > 0 && !g_pool[i].loading &&
		    g_pool[i].in_flight == 0) {
			if (best < 0 || g_pool[i].last_used_ns < best_ts) {
				best = i;
				best_ts = g_pool[i].last_used_ns;
			}
		}
	}
	return best;
}

static int pick_free_port(void)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) { close(fd); return -1; }
	socklen_t len = sizeof addr;
	if (getsockname(fd, (struct sockaddr *)&addr, &len) < 0) { close(fd); return -1; }
	int port = ntohs(addr.sin_port);
	close(fd);
	return port;
}

static char *resolve_model_path(const char *model_name)
{
	const char *url = registry_lookup(model_name);
	char fname_buf[256];
	const char *filename;
	if (url) {
		const char *slash = strrchr(url, '/');
		filename = slash ? slash + 1 : url;
	} else {
		/* manual / non-registry alias: db_model_add stores the alias
		   with the trailing ".gguf" stripped, so we reconstruct here
		   (mirror of main.c's resolve_model_path). */
		size_t nlen = strlen(model_name);
		int has_ext = (nlen >= 5 && strcmp(model_name + nlen - 5, ".gguf") == 0);
		if (has_ext) {
			filename = model_name;
		} else {
			snprintf(fname_buf, sizeof fname_buf, "%s.gguf", model_name);
			filename = fname_buf;
		}
	}
	if (strstr(filename, "..") || strchr(filename, '/')) return NULL;
	char sub[512];
	snprintf(sub, sizeof sub, "models/%s", filename);
	return kora_path(sub);
}

static const char *llama_server_path(void)
{
	static char cached[280];
	if (cached[0]) return cached;
#ifdef __linux__
	char exe[256];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
	if (n > 0) {
		exe[n] = '\0';
		char *slash = strrchr(exe, '/');
		if (slash) {
			*slash = '\0';
			snprintf(cached, sizeof cached, "%s/llama-server", exe);
			if (access(cached, X_OK) == 0) return cached;
		}
	}
#endif
	snprintf(cached, sizeof cached, "llama-server");
	return cached;
}

static int probe_health(int port)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(port);
	if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		close(fd); return -1;
	}
	const char *req = "GET /health HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
	if (write(fd, req, strlen(req)) < 0) { close(fd); return -1; }
	char buf[64] = {0};
	ssize_t r = read(fd, buf, sizeof buf - 1);
	close(fd);
	if (r <= 12) return -1;
	return strstr(buf, " 200 ") ? 1 : 0;
}

static int wait_for_health(int port, int timeout_secs)
{
	for (int i = 0; i < timeout_secs * 10; i++) {
		if (probe_health(port) == 1) return 0;
		struct timespec ts = { 0, 100 * 1000 * 1000 };
		nanosleep(&ts, NULL);
	}
	return -1;
}

/* stop entry i. caller holds g_mu or has exclusive access. */
static void stop_entry(int i)
{
	if (g_pool[i].pid <= 0) return;
	pid_t pid = g_pool[i].pid;
	kill(pid, SIGTERM);
	for (int j = 0; j < 30; j++) {
		int status;
		pid_t r = waitpid(pid, &status, WNOHANG);
		if (r == pid || r < 0) { goto done; }
		struct timespec ts = { 0, 100 * 1000 * 1000 };
		nanosleep(&ts, NULL);
	}
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
done:
	g_pool[i].pid = 0;
	g_pool[i].port = 0;
	g_pool[i].model[0] = '\0';
	g_pool[i].in_flight = 0;
	g_pool[i].loading = 0;
	g_pool[i].last_used_ns = 0;
	g_pool[i].last_request_ns = 0;
}

int kora_pool_ensure_ready(const char *requested, int *out_slot)
{
	const char *model = (requested && *requested) ? requested : g_default_model;
	if (!*model) return -1;

	int slot = -1;
	pthread_mutex_lock(&g_mu);

	for (;;) {
		/* cache hit: bump LRU, bump in_flight, return. */
		int idx = find_loaded(model);
		if (idx >= 0) {
			g_pool[idx].last_used_ns = now_ns();
			g_pool[idx].last_request_ns = now_ns();
			g_pool[idx].in_flight++;
			int port = g_pool[idx].port;
			if (out_slot) *out_slot = idx;
			pthread_mutex_unlock(&g_mu);
			return port;
		}
		/* another thread is loading the same model — wait */
		int loading = find_loading(model);
		if (loading >= 0) {
			pthread_cond_wait(&g_cv, &g_mu);
			continue;
		}
		/* need to load. find a slot. */
		slot = find_empty();
		if (slot < 0) slot = find_lru_evictable();
		if (slot < 0) {
			/* pool full and every entry is in-flight. wait. */
			pthread_cond_wait(&g_cv, &g_mu);
			continue;
		}
		if (g_pool[slot].pid > 0) {
			fprintf(stderr, "kora: evicting %s (LRU)\n", g_pool[slot].model);
			stop_entry(slot);
		}
		g_pool[slot].loading = 1;
		snprintf(g_pool[slot].model, sizeof g_pool[slot].model, "%s", model);
		break;
	}

	int ctx = g_ctx_size;
	int parallel = g_parallel;
	pthread_mutex_unlock(&g_mu);

	/* --- outside the lock: resolve + fork + health probe --- */

	char *gguf = resolve_model_path(model);
	struct stat st;
	if (!gguf || stat(gguf, &st) != 0) {
		fprintf(stderr, "kora: model file missing: %s\n", model);
		free(gguf);
		goto fail;
	}

	int port = pick_free_port();
	if (port <= 0) { free(gguf); goto fail; }

	pid_t pid = fork();
	if (pid < 0) { free(gguf); goto fail; }
	if (pid == 0) {
		char port_s[16], ctx_s[16], par_s[16];
		snprintf(port_s, sizeof port_s, "%d", port);
		snprintf(ctx_s,  sizeof ctx_s,  "%d", ctx);
		snprintf(par_s,  sizeof par_s,  "%d", parallel);
		execl(llama_server_path(), "llama-server",
		      "-m", gguf, "--host", "127.0.0.1",
		      "--port", port_s, "--ctx-size", ctx_s,
		      "--parallel", par_s,
		      "--log-disable",
		      (char *)NULL);
		_exit(127);
	}
	free(gguf);

	if (wait_for_health(port, 60) != 0) {
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		goto fail;
	}

	pthread_mutex_lock(&g_mu);
	g_pool[slot].pid             = pid;
	g_pool[slot].port            = port;
	g_pool[slot].loading         = 0;
	g_pool[slot].last_used_ns    = now_ns();
	g_pool[slot].last_request_ns = now_ns();
	g_pool[slot].in_flight       = 1;  /* this caller */
	pthread_cond_broadcast(&g_cv);
	int ret_port = g_pool[slot].port;
	if (out_slot) *out_slot = slot;
	pthread_mutex_unlock(&g_mu);

	fprintf(stderr, "kora: loaded %s (pid=%d, port=%d, parallel=%d)\n",
	        model, (int)pid, port, parallel);
	return ret_port;

fail:
	pthread_mutex_lock(&g_mu);
	g_pool[slot].loading = 0;
	g_pool[slot].model[0] = '\0';
	pthread_cond_broadcast(&g_cv);
	pthread_mutex_unlock(&g_mu);
	return -1;
}

void kora_pool_release(int slot)
{
	if (slot < 0 || slot >= g_cap) return;
	pthread_mutex_lock(&g_mu);
	if (g_pool[slot].in_flight > 0) g_pool[slot].in_flight--;
	g_pool[slot].last_request_ns = now_ns();
	pthread_cond_broadcast(&g_cv);
	pthread_mutex_unlock(&g_mu);
}

int kora_pool_is_any_running(void)
{
	pthread_mutex_lock(&g_mu);
	int any = 0;
	for (int i = 0; i < g_cap; i++)
		if (g_pool[i].pid > 0) { any = 1; break; }
	pthread_mutex_unlock(&g_mu);
	return any;
}

void kora_pool_idle_check(int timeout_secs)
{
	if (timeout_secs <= 0) return;
	long threshold = (long)timeout_secs * 1000000000L;
	long now = now_ns();
	pthread_mutex_lock(&g_mu);
	for (int i = 0; i < g_cap; i++) {
		if (g_pool[i].pid > 0 && !g_pool[i].loading &&
		    g_pool[i].in_flight == 0 &&
		    now - g_pool[i].last_request_ns >= threshold) {
			fprintf(stderr, "kora: idle-unloading %s (%lds)\n",
			        g_pool[i].model,
			        (now - g_pool[i].last_request_ns) / 1000000000L);
			stop_entry(i);
			pthread_cond_broadcast(&g_cv);
		}
	}
	pthread_mutex_unlock(&g_mu);
}

void kora_pool_stop_all(void)
{
	pthread_mutex_lock(&g_mu);
	for (int i = 0; i < g_cap; i++)
		if (g_pool[i].pid > 0) stop_entry(i);
	pthread_cond_broadcast(&g_cv);
	pthread_mutex_unlock(&g_mu);
}

/* admin helpers */

int kora_pool_unload(const char *model)
{
	pthread_mutex_lock(&g_mu);
	int idx = find_loaded(model);
	if (idx < 0) {
		pthread_mutex_unlock(&g_mu);
		return -1;  /* not loaded */
	}
	if (g_pool[idx].in_flight > 0) {
		pthread_mutex_unlock(&g_mu);
		return -2;  /* busy */
	}
	stop_entry(idx);
	pthread_cond_broadcast(&g_cv);
	pthread_mutex_unlock(&g_mu);
	return 0;
}

/* snapshot pool state into a caller-allocated array. returns count. */
int kora_pool_snapshot(struct kora_pool_snap *out, int cap)
{
	pthread_mutex_lock(&g_mu);
	int n = 0;
	long now = now_ns();
	for (int i = 0; i < g_cap && n < cap; i++) {
		if (g_pool[i].pid <= 0) continue;
		snprintf(out[n].model, sizeof out[n].model, "%s", g_pool[i].model);
		out[n].port = g_pool[i].port;
		out[n].pid = (int)g_pool[i].pid;
		out[n].in_flight = g_pool[i].in_flight;
		out[n].idle_secs = (int)((now - g_pool[i].last_request_ns) / 1000000000L);
		out[n].loading = g_pool[i].loading;
		n++;
	}
	pthread_mutex_unlock(&g_mu);
	return n;
}

int kora_pool_cap(void)       { return g_cap; }
int kora_pool_ctx_size(void)  { return g_ctx_size; }
int kora_pool_parallel(void)  { return g_parallel; }
