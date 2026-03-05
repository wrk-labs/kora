#ifndef KORA_INFERENCE_H
#define KORA_INFERENCE_H

struct kora_ctx {
	struct llama_model *model;
	struct llama_context *ctx;
	struct llama_sampler *sampler;
	const char *chat_template;
	int n_ctx;
};

/* suppress llama.cpp log output */
void kora_suppress_logs(void);

/* load a model from path, returns NULL on failure
   n_ctx=0 auto-detects from model, n_threads=0 auto-detects from hardware */
struct kora_ctx *kora_load(const char *model_path, int n_ctx, int n_gpu_layers, int n_threads);

/* free all resources */
void kora_free(struct kora_ctx *kc);

/* format messages using the model's chat template
   messages is an array of {role, content} pairs, n_msg is the count
   returns allocated string (caller must free), or NULL on error */
char *kora_apply_template(struct kora_ctx *kc,
                          const char **roles, const char **contents,
                          int n_msg);

/* generate a response from a formatted prompt, streaming to stdout
   if out is not NULL, the full response text is stored there (caller must free)
   returns number of tokens generated, or -1 on error */
int kora_generate(struct kora_ctx *kc, const char *prompt, char **out);

/* clear the context (KV cache) for a new conversation */
void kora_clear(struct kora_ctx *kc);

/* print model and context status
   tokens_used=0 means unknown/not computed yet */
void kora_status(struct kora_ctx *kc, const char *model_name, int n_history_msgs, int tokens_used);

/* count tokens in a string */
int kora_token_count(struct kora_ctx *kc, const char *text);

/* check if context is approaching the limit (>90% used)
   prompt_tokens is the total token count of the formatted conversation */
int kora_context_needs_compression(struct kora_ctx *kc, int prompt_tokens);

/* generate a summary of the conversation for context compression
   compact_prompt is the system prompt for summarization
   returns allocated string (caller must free), or NULL on error */
char *kora_summarize(struct kora_ctx *kc, const char *conversation, const char *compact_prompt);

#endif
