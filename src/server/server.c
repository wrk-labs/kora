#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "server.h"
#include "internal.h"

static volatile sig_atomic_t g_shutdown;

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
		kora_pool_idle_check(ia->timeout_secs);
	}
	return NULL;
}

int kora_server_run(const struct kora_server_opts *opts)
{
	if (!opts || !opts->model || !*opts->model) {
		fprintf(stderr, "kora: serve: no default model configured\n");
		return 1;
	}

	g_shutdown = 0;

	kora_pool_init(opts->pool_size > 0 ? opts->pool_size : 2,
	               opts->ctx_size > 0 ? opts->ctx_size : 8192,
	               opts->parallel > 0 ? opts->parallel : 1);
	kora_pool_set_default_model(opts->model);

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
	       "kora: default=%s, pool=%d, parallel=%d, ctx=%d, idle=%ds\n",
	       port, opts->model,
	       opts->pool_size > 0 ? opts->pool_size : 2,
	       opts->parallel > 0 ? opts->parallel : 1,
	       opts->ctx_size > 0 ? opts->ctx_size : 8192,
	       opts->idle_timeout_secs);
	fflush(stdout);

	int rc = kora_proxy_listen(port);

	g_shutdown = 1;
	if (idle_tid)
		pthread_join(idle_tid, NULL);

	kora_pool_stop_all();
	return rc;
}
