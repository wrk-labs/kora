#include <pthread.h>
#include <stdlib.h>

#include "dispatch.h"

static int active = 0;

struct dispatch_ctx {
	void (*fn)(void *arg);
	void *arg;
};

static void *dispatch_thread(void *raw)
{
	struct dispatch_ctx *ctx = raw;

	ctx->fn(ctx->arg);

	active = 0;
	free(ctx);
	return NULL;
}

int dispatch(void (*fn)(void *arg), void *arg)
{
	pthread_t tid;
	struct dispatch_ctx *ctx;

	if (active)
		return -1;

	ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return -1;

	ctx->fn = fn;
	ctx->arg = arg;
	active = 1;

	if (pthread_create(&tid, NULL, dispatch_thread, ctx) != 0) {
		active = 0;
		free(ctx);
		return -1;
	}

	pthread_detach(tid);
	return 0;
}

int dispatch_active(void)
{
	return active;
}
