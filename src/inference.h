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

/* load a model from path, returns NULL on failure */
struct kora_ctx *kora_load(const char *model_path, int n_ctx, int n_gpu_layers);

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

#endif
