#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#include <mach/mach.h>
#else
#include <unistd.h>
#endif

#include "llama.h"
#include "ggml.h"
#include "inference.h"

static uint64_t get_available_memory(void)
{
#ifdef __APPLE__
	/* use free + inactive memory as available */
	mach_port_t host = mach_host_self();
	vm_size_t page_size;
	host_page_size(host, &page_size);

	vm_statistics64_data_t vm_stat;
	mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
	if (host_statistics64(host, HOST_VM_INFO64, (host_info64_t)&vm_stat, &count) != KERN_SUCCESS)
		return 0;

	return ((uint64_t)vm_stat.free_count + (uint64_t)vm_stat.inactive_count) * page_size;
#else
	long pages = sysconf(_SC_AVPHYS_PAGES);
	long page_size = sysconf(_SC_PAGE_SIZE);
	if (pages > 0 && page_size > 0)
		return (uint64_t)pages * (uint64_t)page_size;
	return 0;
#endif
}

/* estimate KV cache memory for a given context size */
static uint64_t estimate_kv_cache(const struct llama_model *model, int n_ctx)
{
	int n_layer = llama_model_n_layer(model);
	int n_head_kv = llama_model_n_head_kv(model);
	int n_embd = llama_model_n_embd(model);
	int n_head = llama_model_n_head(model);
	int head_dim = n_head > 0 ? n_embd / n_head : 128;

	/* 2 (K+V) × layers × kv_heads × head_dim × context × 2 bytes (f16) */
	return (uint64_t)2 * n_layer * n_head_kv * head_dim * n_ctx * 2;
}

/* pick the largest context size that fits in memory */
static int auto_context_size(const struct llama_model *model)
{
	int trained = llama_model_n_ctx_train(model);
	uint64_t avail_mem = get_available_memory();
	if (avail_mem == 0)
		return 4096;

	uint64_t model_mem = llama_model_size(model);

	/* use at most 50% of available memory for KV cache */
	uint64_t budget = model_mem < avail_mem ? avail_mem - model_mem : 0;
	uint64_t available = (uint64_t)(budget * 0.5);
	if (available == 0)
		return 4096;

	/* binary search: find largest power-of-2 friendly context that fits */
	int sizes[] = { 131072, 65536, 32768, 16384, 8192, 4096 };
	int i;
	for (i = 0; i < 6; i++) {
		if (sizes[i] > trained)
			continue;
		if (estimate_kv_cache(model, sizes[i]) <= available)
			return sizes[i];
	}

	return 4096;
}

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

struct kora_ctx *kora_load(const char *model_path, int n_ctx, int n_gpu_layers, int n_threads)
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

	/* context params — auto-detect based on available memory if not overridden */
	struct llama_context_params cparams = llama_context_default_params();
	if (n_ctx > 0)
		kc->n_ctx = n_ctx;
	else
		kc->n_ctx = auto_context_size(kc->model);
	cparams.n_ctx = kc->n_ctx;
	cparams.n_batch = 512;
	if (n_threads > 0) {
		cparams.n_threads = n_threads;
		cparams.n_threads_batch = n_threads;
	}

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
	if (!kc || !kc->ctx)
		return;
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

void kora_status(struct kora_ctx *kc, const char *model_name, int n_history_msgs, int tokens_used)
{
	char buf[128];
	int n;

	printf("\n");
	printf("  Model:        %s\n", model_name);

	/* description/arch from metadata */
	n = llama_model_meta_val_str(kc->model, "general.architecture", buf, sizeof(buf));
	if (n > 0)
		printf("  Architecture: %s\n", buf);

	n = llama_model_meta_val_str(kc->model, "general.quantization_level", buf, sizeof(buf));
	if (n > 0)
		printf("  Quantization: %s\n", buf);

	/* parameter count */
	uint64_t params = llama_model_n_params(kc->model);
	if (params > 0) {
		if (params >= 1000000000)
			printf("  Parameters:   %.1fB\n", (double)params / 1e9);
		else
			printf("  Parameters:   %.0fM\n", (double)params / 1e6);
	}

	int ctx_train = llama_model_n_ctx_train(kc->model);
	if (tokens_used > 0) {
		int pct = (int)((double)tokens_used / kc->n_ctx * 100);
		printf("  Context:      %d / %d tokens (%d%%) (trained: %d)\n",
		       tokens_used, kc->n_ctx, pct, ctx_train);
	} else {
		printf("  Context:      %d tokens (trained: %d)\n", kc->n_ctx, ctx_train);
	}
	printf("  Messages:     %d\n", n_history_msgs);
	printf("\n");
}

int kora_token_count(struct kora_ctx *kc, const char *text)
{
	const struct llama_vocab *vocab = llama_model_get_vocab(kc->model);
	int n = llama_tokenize(vocab, text, strlen(text), NULL, 0, false, false);
	return n < 0 ? -n : n;
}

int kora_context_needs_compression(struct kora_ctx *kc, int prompt_tokens)
{
	int max_output = kc->n_ctx / 4; /* reserve 25% for output */
	int threshold = (int)((kc->n_ctx - max_output) * 0.9);
	return prompt_tokens > threshold;
}

char *kora_summarize(struct kora_ctx *kc, const char *conversation, const char *compact_prompt)
{
	if (!compact_prompt)
		compact_prompt = "Summarize the following conversation concisely.";

	/* build a 2-message conversation: system=compact_prompt, user=conversation */
	const char *roles[2] = { "system", "user" };
	const char *contents[2] = { compact_prompt, conversation };

	char *prompt = kora_apply_template(kc, roles, contents, 2);
	if (!prompt)
		return NULL;

	kora_clear(kc);
	char *summary = NULL;
	int ret = kora_generate(kc, prompt, &summary);
	free(prompt);

	if (ret < 0) {
		free(summary);
		return NULL;
	}

	return summary;
}

/* abort flag */
static volatile int abort_flag = 0;

void kora_abort(void)
{
	abort_flag = 1;
}

void kora_abort_reset(void)
{
	abort_flag = 0;
}

/* streaming callback */
static kora_stream_cb stream_cb = NULL;
static void *stream_cb_data = NULL;

void kora_set_stream_cb(kora_stream_cb cb, void *user_data)
{
	stream_cb = cb;
	stream_cb_data = user_data;
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
		if (abort_flag)
			break;

		llama_token id = llama_sampler_sample(kc->sampler, kc->ctx, -1);

		if (llama_vocab_is_eog(vocab, id))
			break;

		int len = llama_token_to_piece(vocab, id, piece, sizeof(piece), 0, true);
		if (len > 0) {
			/* stream via callback or stdout */
			if (stream_cb) {
				stream_cb(piece, len, stream_cb_data);
			} else {
				fwrite(piece, 1, len, stdout);
				fflush(stdout);
			}

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

	if (!stream_cb)
		printf("\n");
	if (out)
		*out = out_buf;
	return n_gen;
}
