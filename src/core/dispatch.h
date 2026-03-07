#ifndef KORA_DISPATCH_H
#define KORA_DISPATCH_H

/* background task dispatcher
   runs a function in a detached thread. only one task at a time. */

/* dispatch a function to run in a background thread.
   arg is passed to fn and freed by the caller's responsibility
   (typically fn frees it when done).
   returns 0 on success, -1 if a task is already running. */
int dispatch(void (*fn)(void *arg), void *arg);

/* check if a background task is currently running */
int dispatch_active(void);

#endif
