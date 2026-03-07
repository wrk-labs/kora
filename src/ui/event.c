#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "event.h"

#define QUEUE_SIZE 512

static struct tui_event queue[QUEUE_SIZE];
static int head = 0;
static int tail = 0;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void event_init(void)
{
	pthread_mutex_lock(&lock);
	head = 0;
	tail = 0;
	pthread_mutex_unlock(&lock);
}

void event_cleanup(void)
{
	pthread_mutex_lock(&lock);
	/* free any unconsumed events */
	while (tail != head) {
		free(queue[tail].data);
		queue[tail].data = NULL;
		tail = (tail + 1) % QUEUE_SIZE;
	}
	head = 0;
	tail = 0;
	pthread_mutex_unlock(&lock);
}

void event_push(const struct tui_event *ev)
{
	int next;

	pthread_mutex_lock(&lock);
	next = (head + 1) % QUEUE_SIZE;
	if (next != tail) {
		queue[head].type = ev->type;
		queue[head].len = ev->len;
		queue[head].data = ev->data ? strdup(ev->data) : NULL;
		head = next;
	}
	/* drop event if queue is full */
	pthread_mutex_unlock(&lock);
}

int event_poll(struct tui_event *ev)
{
	pthread_mutex_lock(&lock);
	if (tail == head) {
		pthread_mutex_unlock(&lock);
		return 0;
	}
	*ev = queue[tail];
	queue[tail].data = NULL;  /* ownership transferred to caller */
	tail = (tail + 1) % QUEUE_SIZE;
	pthread_mutex_unlock(&lock);
	return 1;
}
