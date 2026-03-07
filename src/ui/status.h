#ifndef KORA_STATUS_H
#define KORA_STATUS_H

#include <stddef.h>

/* status handler IDs — higher ID = higher display priority.
   only the highest-priority active handler is shown. */
#define STATUS_CONTEXT  0
#define STATUS_DOWNLOAD 1

/* callback for poll-mode handlers: writes status text into buf */
typedef void (*status_fn)(char *buf, size_t bufsize, void *data);

/* wire a status handler.
   poll mode: fn != NULL, interval_ms > 0 — fn called at interval.
   push mode: fn = NULL — use status_set() to update text. */
void status_wire(int id, status_fn fn, void *data, int interval_ms);

/* unwire a handler — next lower-priority handler resumes */
void status_unwire(int id);

/* push-mode: set handler text directly (thread-safe) */
void status_set(int id, const char *text);

/* called from main loop. polls handlers, returns current display text.
   sets *changed to 1 if text differs from last call. */
const char *status_tick(int *changed);

#endif
