#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inference.h"
#include "lua_bridge.h"
#include "model.h"
#include "registry.h"
#include "util.h"

static void usage(void)
{
	printf("usage: kora <command> [args]\n"
	       "\n"
	       "commands:\n"
	       "  chat            interactive chat\n"
	       "  code            agentic coding assistant\n"
	       "  pull <model>    download a model\n"
	       "  list            list downloaded models\n"
	       "  rm <model>      remove a model\n"
	       "  sessions        list sessions\n"
	       "  version         print version\n");
}

static void version(void)
{
	printf("kora %s\n", VERSION);
}

/* resolve a model alias to a file path in ~/.kora/models/ */
static char *resolve_model_path(const char *model_name)
{
	const char *url = registry_lookup(model_name);
	const char *filename;
	if (url) {
		const char *slash = strrchr(url, '/');
		filename = slash ? slash + 1 : url;
	} else {
		filename = model_name;
	}

	char sub[512];
	snprintf(sub, sizeof(sub), "models/%s", filename);
	return kora_path(sub);
}

int main(int argc, char *argv[])
{
	if (argc < 2) {
		usage();
		return 1;
	}

	if (strcmp(argv[1], "version") == 0 || strcmp(argv[1], "--version") == 0) {
		version();
		return 0;
	}

	if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0) {
		usage();
		return 0;
	}

	if (kora_init_dirs() != 0) {
		fprintf(stderr, "kora: failed to initialize ~/.kora/\n");
		return 1;
	}

	if (strcmp(argv[1], "pull") == 0) {
		if (argc < 3) {
			fprintf(stderr, "usage: kora pull <model|url>\n");
			return 1;
		}
		return model_pull(argv[2]);
	}

	if (strcmp(argv[1], "list") == 0) {
		return model_list();
	}

	if (strcmp(argv[1], "rm") == 0) {
		if (argc < 3) {
			fprintf(stderr, "usage: kora rm <model>\n");
			return 1;
		}
		return model_rm(argv[2]);
	}

	if (strcmp(argv[1], "chat") == 0) {
		/* load config */
		struct kora_config *cfg = kora_config_load(LUADIR);

		/* resolve model: -m flag > chat_model > default_model */
		const char *model_name = cfg->chat_model ? cfg->chat_model : cfg->default_model;
		int i;
		for (i = 2; i < argc - 1; i++) {
			if (strcmp(argv[i], "-m") == 0)
				model_name = argv[i + 1];
		}

		char *model_path = resolve_model_path(model_name);
		if (!model_path) {
			kora_config_free(cfg);
			return 1;
		}

		kora_suppress_logs();
		printf("loading %s...\n", model_name);

		struct kora_ctx *kc = kora_load(model_path, cfg->ctx_size, cfg->gpu_layers);
		free(model_path);
		if (!kc) {
			kora_config_free(cfg);
			return 1;
		}

		/* conversation history */
		#define MAX_TURNS 128
		const char *roles[1 + MAX_TURNS * 2];
		const char *contents[1 + MAX_TURNS * 2];
		char *history_bufs[MAX_TURNS * 2];
		int n_msg = 0;
		int n_hist = 0;

		roles[0] = "system";
		contents[0] = cfg->system_chat;
		n_msg = 1;

		char input[4096];
		printf("kora chat (%s) — type 'exit' to quit\n\n", model_name);
		while (1) {
			printf("> ");
			fflush(stdout);
			if (!fgets(input, sizeof(input), stdin))
				break;
			input[strcspn(input, "\n")] = 0;
			if (strcmp(input, "exit") == 0 || strcmp(input, "quit") == 0)
				break;
			if (input[0] == '\0')
				continue;

			if (n_msg >= 1 + MAX_TURNS * 2 - 1) {
				fprintf(stderr, "kora: conversation too long, start a new session\n");
				break;
			}

			/* add user message */
			history_bufs[n_hist] = strdup(input);
			roles[n_msg] = "user";
			contents[n_msg] = history_bufs[n_hist];
			n_msg++;
			n_hist++;

			/* apply template with full conversation */
			char *prompt = kora_apply_template(kc, roles, contents, n_msg);
			if (!prompt) {
				fprintf(stderr, "kora: failed to apply chat template\n");
				n_msg--;
				n_hist--;
				free(history_bufs[--n_hist]);
				continue;
			}

			/* regenerate from full history */
			kora_clear(kc);
			char *response = NULL;
			kora_generate(kc, prompt, &response);
			free(prompt);

			/* add assistant response */
			if (response) {
				history_bufs[n_hist] = response;
				roles[n_msg] = "assistant";
				contents[n_msg] = history_bufs[n_hist];
				n_msg++;
				n_hist++;
			}
			printf("\n");
		}

		for (i = 0; i < n_hist; i++)
			free(history_bufs[i]);
		kora_free(kc);
		kora_config_free(cfg);
		return 0;
	}

	if (strcmp(argv[1], "code") == 0) {
		printf("code: not yet implemented\n");
		return 1;
	}

	if (strcmp(argv[1], "sessions") == 0) {
		printf("sessions: not yet implemented\n");
		return 1;
	}

	fprintf(stderr, "kora: unknown command '%s'\n", argv[1]);
	usage();
	return 1;
}
