#ifndef KORA_CLIENT_H
#define KORA_CLIENT_H

struct kora_message {
	const char *role;     /* "system" | "user" | "assistant" */
	const char *content;
};

typedef void (*kora_client_chunk_cb)(const char *text, int len, void *user_data);

struct kora_client_chat_opts {
	const char *base_url;           /* e.g. "http://127.0.0.1:8818" */
	const char *model;
	const struct kora_message *msgs;
	int n_msgs;
	int max_tokens;                 /* 0 = server default */
	kora_client_chunk_cb chunk_cb;  /* optional; called per streamed token chunk */
	void *chunk_user_data;
	volatile int *cancel;           /* optional; non-zero aborts the stream */
};

/* streaming chat completion. blocks until stream ends or is cancelled.
   writes the full assistant response to *out (caller frees). returns 0 on
   success (including clean cancel), -1 on transport/protocol error. */
int kora_client_chat(const struct kora_client_chat_opts *opts, char **out);

/* token count for text, via daemon's /tokenize. returns count, -1 on error. */
int kora_client_count_tokens(const char *base_url, const char *model, const char *text);

/* GET /kora/status; returns 0 if daemon is reachable and healthy. */
int kora_client_ping(const char *base_url);

/* extract the value of the first `"content":"..."` pair from a JSON object,
   unescaping \n \t \" \uXXXX (BMP) etc into `out`. returns the number of
   bytes written (excluding the NUL terminator), or -1 if `"content"` is
   missing / malformed. `out_cap` must be > 0. exposed for unit tests. */
int kora_json_extract_content(const char *json, char *out, int out_cap);

#endif
