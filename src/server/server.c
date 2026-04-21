#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "server.h"
#include "internal.h"

static _Atomic long g_last_request_ns;
static _Atomic int  g_in_flight;
static volatile sig_atomic_t g_shutdown;

static long now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000000000L + ts.tv_nsec;
}

void kora_server_begin_request(void)
{
	atomic_store(&g_last_request_ns, now_ns());
	atomic_fetch_add(&g_in_flight, 1);
}

void kora_server_end_request(void)
{
	atomic_store(&g_last_request_ns, now_ns());
	atomic_fetch_sub(&g_in_flight, 1);
}

static void on_signal(int sig)
{
	(void)sig;
	g_shutdown = 1;
	kora_proxy_stop();
}

struct idle_args {
	int timeout_secs;
};

static void *idle_watcher(void *arg)
{
	const struct idle_args *ia = arg;
	while (!g_shutdown) {
		sleep(10);
		if (g_shutdown) break;
		if (!kora_child_is_running()) continue;
		if (atomic_load(&g_in_flight) > 0) continue;
		long age = now_ns() - atomic_load(&g_last_request_ns);
		if (age / 1000000000L >= ia->timeout_secs) {
			fprintf(stderr, "kora: idle for %lds, unloading model\n",
			        age / 1000000000L);
			kora_child_stop();
		}
	}
	return NULL;
}

int kora_server_run(const struct kora_server_opts *opts)
{
	if (!opts || !opts->model || !*opts->model) {
		fprintf(stderr, "kora: serve: no default model configured\n");
		return 1;
	}

	atomic_store(&g_last_request_ns, now_ns());
	atomic_store(&g_in_flight, 0);
	g_shutdown = 0;

	kora_child_init(opts->ctx_size > 0 ? opts->ctx_size : 8192);
	kora_child_set_default_model(opts->model);

	struct sigaction sa = {0};
	sa.sa_handler = on_signal;
	sigaction(SIGINT,  &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	pthread_t idle_tid = 0;
	struct idle_args ia = { opts->idle_timeout_secs };
	if (opts->idle_timeout_secs > 0)
		pthread_create(&idle_tid, NULL, idle_watcher, &ia);

	int port = opts->public_port > 0 ? opts->public_port : 8818;
	printf("kora: supervisor listening on http://127.0.0.1:%d\n"
	       "kora: default model = %s, idle_timeout = %ds\n",
	       port, opts->model, opts->idle_timeout_secs);
	fflush(stdout);

	int rc = kora_proxy_listen(port);

	g_shutdown = 1;
	if (idle_tid)
		pthread_join(idle_tid, NULL);

	kora_child_stop();
	return rc;
}
