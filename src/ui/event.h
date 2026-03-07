#ifndef KORA_EVENT_H
#define KORA_EVENT_H

/* lightweight event queue for TUI components.
   background threads push events; the main loop drains and renders. */

enum tui_event_type {
	TUI_EV_NONE = 0,
	TUI_EV_INFO,
	TUI_EV_STATUSBAR_LEFT,
	TUI_EV_CHAT_BEGIN,
	TUI_EV_CHAT_CHUNK,
	TUI_EV_CHAT_END,
	TUI_EV_USER_MSG,
	TUI_EV_SPINNER,
};

struct tui_event {
	enum tui_event_type type;
	char *data;  /* heap-allocated, owned by the event */
	int len;     /* chunk byte length, or on/off for spinner */
};

void event_init(void);
void event_cleanup(void);

/* thread-safe: push an event (copies data) */
void event_push(const struct tui_event *ev);

/* main thread only: pop next event, returns 1 if available.
   caller must free ev->data when done. */
int event_poll(struct tui_event *ev);

#endif
