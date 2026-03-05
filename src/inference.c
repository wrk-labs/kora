#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llama.h"
#include "ggml.h"
#include "inference.h"

static void log_noop(enum ggml_log_level level, const char *text, void *user_data)
{
	(void)level;
	(void)text;
	(void)user_data;
}

void kora_suppress_logs(void)
{
	llama_log_set(log_noop, NULL);
}

struct kora_ctx *kora_load(const char *model_path, int n_ctx, int n_gpu_layers)
{
	struct kora_ctx *kc = calloc(1, sizeof(*kc));
	if (!kc)
		return NULL;

	/* model params */
	struct llama_model_params mparams = llama_model_default_params();
	mparams.n_gpu_layers = n_gpu_layers;

	kc->model = llama_model_load_from_file(model_path, mparams);
	if (!kc->model) {
		fprintf(stderr, "kora: failed to load model '%s'\n", model_path);
		free(kc);
		return NULL;
	}

	/* read chat template from model metadata */
	kc->chat_template = llama_model_chat_template(kc->model, NULL);

	/* context params */
	struct llama_context_params cparams = llama_context_default_params();
	kc->n_ctx = n_ctx > 0 ? n_ctx : 4096;
	cparams.n_ctx = kc->n_ctx;
	cparams.n_batch = 512;

	kc->ctx = llama_init_from_model(kc->model, cparams);
	if (!kc->ctx) {
		fprintf(stderr, "kora: failed to create context\n");
		llama_model_free(kc->model);
		free(kc);
		return NULL;
	}

	/* sampler: temp -> top-p -> top-k -> repetition penalty */
	struct llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
	kc->sampler = llama_sampler_chain_init(sparams);
	llama_sampler_chain_add(kc->sampler, llama_sampler_init_temp(0.7f));
	llama_sampler_chain_add(kc->sampler, llama_sampler_init_top_p(0.9f, 1));
	llama_sampler_chain_add(kc->sampler, llama_sampler_init_top_k(40));
	llama_sampler_chain_add(kc->sampler, llama_sampler_init_dist(0));
	llama_sampler_chain_add(kc->sampler, llama_sampler_init_penalties(
		64, 1.1f, 0.0f, 0.0f));

	return kc;
}

void kora_free(struct kora_ctx *kc)
{
	if (!kc)
		return;
	if (kc->sampler)
		llama_sampler_free(kc->sampler);
	if (kc->ctx)
		llama_free(kc->ctx);
	if (kc->model)
		llama_model_free(kc->model);
	free(kc);
}

void kora_clear(struct kora_ctx *kc)
{
	llama_memory_clear(llama_get_memory(kc->ctx), true);
}

char *kora_apply_template(struct kora_ctx *kc,
                          const char **roles, const char **contents,
                          int n_msg)
{
	/* build llama_chat_message array */
	struct llama_chat_message *msgs = malloc(n_msg * sizeof(struct llama_chat_message));
	if (!msgs)
		return NULL;

	int i;
	for (i = 0; i < n_msg; i++) {
		msgs[i].role = roles[i];
		msgs[i].content = contents[i];
	}

	/* first call to get required buffer size */
	int len = llama_chat_apply_template(kc->chat_template, msgs, n_msg, true, NULL, 0);
	if (len < 0) {
		free(msgs);
		return NULL;
	}

	char *buf = malloc(len + 1);
	if (!buf) {
		free(msgs);
		return NULL;
	}

	llama_chat_apply_template(kc->chat_template, msgs, n_msg, true, buf, len + 1);
	buf[len] = '\0';
	free(msgs);
	return buf;
}

int kora_generate(struct kora_ctx *kc, const char *prompt, char **out)
{
	const struct llama_vocab *vocab = llama_model_get_vocab(kc->model);

	/* tokenize prompt */
	int n_prompt = llama_tokenize(vocab, prompt, strlen(prompt), NULL, 0, true, true);
	if (n_prompt < 0)
		n_prompt = -n_prompt;

	llama_token *tokens = malloc(n_prompt * sizeof(llama_token));
	if (!tokens)
		return -1;

	llama_tokenize(vocab, prompt, strlen(prompt), tokens, n_prompt, true, true);

	/* decode prompt in chunks of n_batch */
	int n_batch = 512;
	int pos;
	for (pos = 0; pos < n_prompt; pos += n_batch) {
		int n = n_prompt - pos;
		if (n > n_batch)
			n = n_batch;
		struct llama_batch batch = llama_batch_get_one(tokens + pos, n);
		if (llama_decode(kc->ctx, batch) != 0) {
			fprintf(stderr, "kora: failed to decode prompt\n");
			free(tokens);
			return -1;
		}
	}
	free(tokens);

	/* output buffer */
	size_t out_cap = 4096;
	size_t out_len = 0;
	char *out_buf = NULL;
	if (out) {
		out_buf = malloc(out_cap);
		if (!out_buf)
			return -1;
		out_buf[0] = '\0';
	}

	/* generate tokens */
	int n_gen = 0;
	int n_max = kc->n_ctx - n_prompt;
	char piece[128];

	while (n_gen < n_max) {
		llama_token id = llama_sampler_sample(kc->sampler, kc->ctx, -1);

		if (llama_vocab_is_eog(vocab, id))
			break;

		int len = llama_token_to_piece(vocab, id, piece, sizeof(piece), 0, true);
		if (len > 0) {
			fwrite(piece, 1, len, stdout);
			fflush(stdout);

			/* accumulate output */
			if (out_buf) {
				while (out_len + len + 1 > out_cap) {
					out_cap *= 2;
					out_buf = realloc(out_buf, out_cap);
					if (!out_buf)
						return -1;
				}
				memcpy(out_buf + out_len, piece, len);
				out_len += len;
				out_buf[out_len] = '\0';
			}
		}

		struct llama_batch batch = llama_batch_get_one(&id, 1);
		if (llama_decode(kc->ctx, batch) != 0) {
			fprintf(stderr, "\nkora: decode error\n");
			free(out_buf);
			return -1;
		}

		n_gen++;
	}

	printf("\n");
	if (out)
		*out = out_buf;
	return n_gen;
}
