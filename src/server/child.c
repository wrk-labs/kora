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

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pid_t g_pid;
static int   g_port;
static char  g_model[128];
static int   g_ctx_size = 8192;
static char  g_default_model[128];

void kora_child_init(int ctx_size)
{
	if (ctx_size > 0) g_ctx_size = ctx_size;
}

void kora_child_set_default_model(const char *m)
{
	snprintf(g_default_model, sizeof g_default_model, "%s", m ? m : "");
}

int kora_child_is_running(void)
{
	pthread_mutex_lock(&g_mu);
	int running = g_pid > 0;
	pthread_mutex_unlock(&g_mu);
	return running;
}

/* bind to 127.0.0.1:0, read the assigned port, close. returns -1 on failure. */
static int pick_free_port(void)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		close(fd); return -1;
	}
	socklen_t len = sizeof addr;
	if (getsockname(fd, (struct sockaddr *)&addr, &len) < 0) {
		close(fd); return -1;
	}
	int port = ntohs(addr.sin_port);
	close(fd);
	return port;
}

/* resolve an alias through the registry; reject traversal. caller frees. */
static char *resolve_model_path(const char *model_name)
{
	const char *url = registry_lookup(model_name);
	const char *filename;
	if (url) {
		const char *slash = strrchr(url, '/');
		filename = slash ? slash + 1 : url;
	} else {
		filename = model_name;
	}
	if (strstr(filename, "..") || strchr(filename, '/')) return NULL;
	char sub[512];
	snprintf(sub, sizeof sub, "models/%s", filename);
	return kora_path(sub);
}

/* find llama-server next to the current executable; fall back to PATH. */
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

/* one-shot HTTP GET /health to 127.0.0.1:port. returns 1 if responded 200,
   0 if still loading (503), -1 on connection failure. */
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
	ssize_t w = write(fd, req, strlen(req));
	if (w < 0) { close(fd); return -1; }
	char buf[64] = {0};
	ssize_t r = read(fd, buf, sizeof buf - 1);
	close(fd);
	if (r <= 12) return -1;
	if (strstr(buf, " 200 ")) return 1;
	return 0;
}

/* poll probe_health up to timeout_secs at 10Hz. returns 0 on ready, -1 on timeout. */
static int wait_for_health(int port, int timeout_secs)
{
	for (int i = 0; i < timeout_secs * 10; i++) {
		int r = probe_health(port);
		if (r == 1) return 0;
		struct timespec ts = { 0, 100 * 1000 * 1000 };
		nanosleep(&ts, NULL);
	}
	return -1;
}

/* must be called with g_mu held */
static void stop_locked(void)
{
	if (g_pid <= 0) return;
	kill(g_pid, SIGTERM);
	for (int i = 0; i < 30; i++) {
		int status;
		pid_t r = waitpid(g_pid, &status, WNOHANG);
		if (r == g_pid || r < 0) { g_pid = 0; break; }
		struct timespec ts = { 0, 100 * 1000 * 1000 };
		nanosleep(&ts, NULL);
	}
	if (g_pid > 0) {
		kill(g_pid, SIGKILL);
		waitpid(g_pid, NULL, 0);
		g_pid = 0;
	}
	g_port = 0;
	g_model[0] = '\0';
}

void kora_child_stop(void)
{
	pthread_mutex_lock(&g_mu);
	stop_locked();
	pthread_mutex_unlock(&g_mu);
}

int kora_child_ensure_ready(const char *requested)
{
	const char *model = (requested && *requested) ? requested : g_default_model;
	if (!*model) return -1;

	pthread_mutex_lock(&g_mu);

	if (g_pid > 0 && strcmp(g_model, model) == 0) {
		int port = g_port;
		pthread_mutex_unlock(&g_mu);
		return port;
	}
	if (g_pid > 0) stop_locked();

	char *gguf = resolve_model_path(model);
	if (!gguf) {
		pthread_mutex_unlock(&g_mu);
		fprintf(stderr, "kora: can't resolve model '%s'\n", model);
		return -1;
	}
	struct stat st;
	if (stat(gguf, &st) != 0) {
		fprintf(stderr, "kora: model file missing: %s\n", gguf);
		free(gguf);
		pthread_mutex_unlock(&g_mu);
		return -1;
	}

	int port = pick_free_port();
	if (port <= 0) {
		free(gguf);
		pthread_mutex_unlock(&g_mu);
		return -1;
	}

	pid_t pid = fork();
	if (pid < 0) {
		free(gguf);
		pthread_mutex_unlock(&g_mu);
		return -1;
	}
	if (pid == 0) {
		char port_s[16], ctx_s[16];
		snprintf(port_s, sizeof port_s, "%d", port);
		snprintf(ctx_s,  sizeof ctx_s,  "%d", g_ctx_size);
		execl(llama_server_path(), "llama-server",
		      "-m", gguf, "--host", "127.0.0.1",
		      "--port", port_s, "--ctx-size", ctx_s,
		      "--log-disable",
		      (char *)NULL);
		_exit(127);
	}
	free(gguf);

	if (wait_for_health(port, 60) != 0) {
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		pthread_mutex_unlock(&g_mu);
		fprintf(stderr, "kora: llama-server failed to come up "
		        "(model=%s, port=%d)\n", model, port);
		return -1;
	}

	g_pid = pid;
	g_port = port;
	snprintf(g_model, sizeof g_model, "%s", model);
	fprintf(stderr, "kora: loaded %s (pid=%d, port=%d)\n",
	        model, (int)pid, port);
	pthread_mutex_unlock(&g_mu);
	return port;
}
