#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "status.h"

#define MAX_HANDLERS 8

struct status_handler {
	int active;
	status_fn fn;
	void *data;
	int interval_ms;
	char *text;
	struct timespec last_tick;
};

static struct status_handler handlers[MAX_HANDLERS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char tick_buf[512];
static char prev_buf[512];

static int elapsed_ms(struct timespec *start)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int)((now.tv_sec - start->tv_sec) * 1000 +
	             (now.tv_nsec - start->tv_nsec) / 1000000);
}

void status_wire(int id, status_fn fn, void *data, int interval_ms)
{
	if (id < 0 || id >= MAX_HANDLERS) return;

	pthread_mutex_lock(&lock);
	handlers[id].active = 1;
	handlers[id].fn = fn;
	handlers[id].data = data;
	handlers[id].interval_ms = interval_ms;
	free(handlers[id].text);
	handlers[id].text = NULL;
	clock_gettime(CLOCK_MONOTONIC, &handlers[id].last_tick);
	pthread_mutex_unlock(&lock);
}

void status_unwire(int id)
{
	if (id < 0 || id >= MAX_HANDLERS) return;

	pthread_mutex_lock(&lock);
	handlers[id].active = 0;
	handlers[id].fn = NULL;
	handlers[id].data = NULL;
	free(handlers[id].text);
	handlers[id].text = NULL;
	pthread_mutex_unlock(&lock);
}

void status_set(int id, const char *text)
{
	if (id < 0 || id >= MAX_HANDLERS) return;

	pthread_mutex_lock(&lock);
	free(handlers[id].text);
	handlers[id].text = text ? strdup(text) : NULL;
	pthread_mutex_unlock(&lock);
}

const char *status_tick(int *changed)
{
	int i;
	const char *result = NULL;

	pthread_mutex_lock(&lock);

	/* poll handlers that need updating */
	for (i = 0; i < MAX_HANDLERS; i++) {
		if (!handlers[i].active || !handlers[i].fn || handlers[i].interval_ms <= 0)
			continue;
		if (elapsed_ms(&handlers[i].last_tick) >= handlers[i].interval_ms) {
			char buf[256];
			buf[0] = '\0';
			handlers[i].fn(buf, sizeof(buf), handlers[i].data);
			free(handlers[i].text);
			handlers[i].text = buf[0] ? strdup(buf) : NULL;
			clock_gettime(CLOCK_MONOTONIC, &handlers[i].last_tick);
		}
	}

	/* find highest-priority active handler with text */
	for (i = MAX_HANDLERS - 1; i >= 0; i--) {
		if (handlers[i].active && handlers[i].text) {
			result = handlers[i].text;
			break;
		}
	}

	/* copy to stable buffer */
	if (result)
		snprintf(tick_buf, sizeof(tick_buf), "%s", result);
	else
		tick_buf[0] = '\0';

	pthread_mutex_unlock(&lock);

	/* detect change */
	if (changed) {
		*changed = strcmp(tick_buf, prev_buf) != 0;
		if (*changed)
			snprintf(prev_buf, sizeof(prev_buf), "%s", tick_buf);
	}

	return tick_buf[0] ? tick_buf : NULL;
}
