#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input.h"
#include "inference.h"
#include "lua_bridge.h"
#include "model.h"
#include "registry.h"
#include "util.h"

static void usage(void)
{
	printf("Usage: kora <command> [args]\n"
	       "\n"
	       "Commands:\n"
	       "  chat            Interactive chat\n"
	       "  code            Agentic coding assistant\n"
	       "  pull <model>    Download a model\n"
	       "  list            List downloaded models\n"
	       "  rm <model>      Remove a model\n"
	       "  sessions        List sessions\n"
	       "  version         Print version\n");
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

	/* reject path traversal */
	if (strstr(filename, "..") || strchr(filename, '/')) {
		fprintf(stderr, "kora: invalid model name '%s'\n", model_name);
		return NULL;
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
			fprintf(stderr, "Usage: kora pull <model|url>\n");
			return 1;
		}
		return model_pull(argv[2]);
	}

	if (strcmp(argv[1], "list") == 0) {
		return model_list();
	}

	if (strcmp(argv[1], "rm") == 0) {
		if (argc < 3) {
			fprintf(stderr, "Usage: kora rm <model>\n");
			return 1;
		}
		return model_rm(argv[2]);
	}

	if (strcmp(argv[1], "chat") == 0) {
		/* load config */
		struct kora_config *cfg = kora_config_load(LUADIR);

		/* resolve model: arg > -m flag > preferred_model */
		const char *model_name = NULL;
		char *preferred = NULL;
		int i;

		/* kora chat <model> */
		if (argc >= 3 && argv[2][0] != '-')
			model_name = argv[2];

		/* kora chat -m <model> */
		for (i = 2; i < argc - 1; i++) {
			if (strcmp(argv[i], "-m") == 0)
				model_name = argv[i + 1];
		}

		/* fallback to preferred model */
		if (!model_name) {
			preferred = kora_preferred_model();
			if (preferred)
				model_name = preferred;
		}

		/* no model specified and no preferred model */
		if (!model_name) {
			printf("No model selected.\n\n");
			printf("Downloaded models:\n");
			model_list();
			printf("\nUsage: kora chat <model>\n");
			printf("To download a model: kora pull <model>\n");
			free(preferred);
			kora_config_free(cfg);
			return 1;
		}

		char *model_path = resolve_model_path(model_name);
		if (!model_path) {
			free(preferred);
			kora_config_free(cfg);
			return 1;
		}

		/* check model file exists */
		if (!model_exists(model_path)) {
			fprintf(stderr, "Model '%s' not found.\n\n", model_name);
			printf("Downloaded models:\n");
			model_list();
			printf("\nTo download: kora pull %s\n", model_name);
			free(model_path);
			free(preferred);
			kora_config_free(cfg);
			return 1;
		}

		/* save as preferred model and keep a stable copy of the name */
		kora_set_preferred_model(model_name);
		char *current_model = strdup(model_name);
		free(preferred);
		preferred = NULL;
		model_name = current_model;

		kora_suppress_logs();
		printf("Loading %s...\n", model_name);

		struct kora_ctx *kc = kora_load(model_path, cfg->ctx_size, cfg->gpu_layers, cfg->threads);
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

		if (cfg->system_chat) {
			roles[0] = "system";
			contents[0] = cfg->system_chat;
			n_msg = 1;
		} else {
			n_msg = 0;
		}

		input_init();
		kora_status(kc, model_name, 0, 0);
		printf("Type /help for commands.\n\n");
		int running = 1;
		while (running) {
			char *input = input_read("> ");
			if (!input)
				break;
			if (input[0] == '\0') {
				free(input);
				continue;
			}

			/* slash commands */
			if (strcmp(input, "/exit") == 0 || strcmp(input, "exit") == 0 ||
			    strcmp(input, "/quit") == 0 || strcmp(input, "quit") == 0) {
				free(input);
				running = 0;
			} else if (strcmp(input, "/help") == 0) {
				printf("\nCommands:\n"
				       "  /help           Show this message\n"
				       "  /model <name>   Switch to a different model\n"
				       "  /status         Show model and context info\n"
				       "  /compact        Summarize conversation to free context\n"
				       "  /clear          Clear conversation history\n"
				       "  /exit           Quit chat\n\n");
				free(input);
			} else if (strcmp(input, "/status") == 0) {
				char *full_prompt = kora_apply_template(kc, roles, contents, n_msg);
				int used = full_prompt ? kora_token_count(kc, full_prompt) : 0;
				free(full_prompt);
				kora_status(kc, model_name, n_msg - 1, used);
				free(input);
			} else if (strcmp(input, "/clear") == 0) {
				for (i = 0; i < n_hist; i++)
					free(history_bufs[i]);
				n_msg = 1;
				n_hist = 0;
				kora_clear(kc);
				printf("Conversation cleared.\n");
				free(input);
			} else if (strcmp(input, "/compact") == 0) {
				if (n_msg <= 1) {
					printf("Nothing to compact.\n");
				} else {
					char *full_prompt = kora_apply_template(kc, roles, contents, n_msg);
					if (full_prompt) {
						int before = kora_token_count(kc, full_prompt);
						printf("Compacting context (%d tokens)...\n", before);
						char *summary = kora_summarize(kc, full_prompt, cfg->compact_chat);
						free(full_prompt);

						if (summary) {
							for (i = 0; i < n_hist; i++)
								free(history_bufs[i]);
							n_hist = 0;
							n_msg = 1;

							history_bufs[n_hist] = summary;
							roles[n_msg] = "assistant";
							contents[n_msg] = history_bufs[n_hist];
							n_msg++;
							n_hist++;

							char *after_prompt = kora_apply_template(kc, roles, contents, n_msg);
							int after = after_prompt ? kora_token_count(kc, after_prompt) : 0;
							free(after_prompt);
							printf("Compacted: %d → %d tokens\n", before, after);
						} else {
							fprintf(stderr, "kora: compaction failed\n");
						}
					}
				}
				free(input);
			} else if (strncmp(input, "/model", 6) == 0) {
				const char *new_name = input + 6;
				while (*new_name == ' ')
					new_name++;
				if (*new_name == '\0') {
					printf("Current model: %s\n", model_name);
					printf("Usage: /model <name>\n");
				} else {
					char *new_path = resolve_model_path(new_name);
					if (!new_path) {
						fprintf(stderr, "kora: could not resolve model '%s'\n", new_name);
					} else {
						printf("Loading %s...\n", new_name);
						struct kora_ctx *new_kc = kora_load(new_path, cfg->ctx_size, cfg->gpu_layers, cfg->threads);
						free(new_path);
						if (!new_kc) {
							fprintf(stderr, "kora: failed to load '%s', keeping current model\n", new_name);
						} else {
							kora_free(kc);
							kc = new_kc;
							free(current_model);
							current_model = strdup(new_name);
							model_name = current_model;
							kora_set_preferred_model(model_name);
							printf("Switched to %s\n\n", model_name);
						}
					}
				}
				free(input);
			} else if (input[0] == '/') {
				fprintf(stderr, "Unknown command: %s (type /help)\n", input);
				free(input);
			} else if (n_msg >= 1 + MAX_TURNS * 2 - 1) {
				fprintf(stderr, "kora: conversation too long, use /clear or /exit\n");
				free(input);
				running = 0;
			} else {
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
					free(input);
					continue;
				}

				/* compress context if approaching the limit */
				int prompt_tokens = kora_token_count(kc, prompt);
				if (kora_context_needs_compression(kc, prompt_tokens)) {
					printf("[Compressing context...]\n");
					char *summary = kora_summarize(kc, prompt, cfg->compact_chat);
					free(prompt);

					if (summary) {
						for (i = 0; i < n_hist; i++)
							free(history_bufs[i]);

						char *user_msg = strdup(input);
						n_hist = 0;
						n_msg = 1;

						history_bufs[n_hist] = summary;
						roles[n_msg] = "assistant";
						contents[n_msg] = history_bufs[n_hist];
						n_msg++;
						n_hist++;

						history_bufs[n_hist] = user_msg;
						roles[n_msg] = "user";
						contents[n_msg] = history_bufs[n_hist];
						n_msg++;
						n_hist++;
					}

					prompt = kora_apply_template(kc, roles, contents, n_msg);
					if (!prompt) {
						fprintf(stderr, "kora: failed to apply template after compression\n");
						free(input);
						continue;
					}
				}

				/* regenerate from full history */
				kora_clear(kc);
				char *response = NULL;
				kora_generate(kc, prompt, &response);
				free(prompt);

				if (response) {
					history_bufs[n_hist] = response;
					roles[n_msg] = "assistant";
					contents[n_msg] = history_bufs[n_hist];
					n_msg++;
					n_hist++;
				}
				printf("\n");
				free(input);
			}
		}

		for (i = 0; i < n_hist; i++)
			free(history_bufs[i]);
		input_cleanup();
		free(current_model);
		kora_free(kc);
		kora_config_free(cfg);
		return 0;
	}

	if (strcmp(argv[1], "code") == 0) {
		printf("Code: not yet implemented\n");
		return 1;
	}

	if (strcmp(argv[1], "sessions") == 0) {
		printf("Sessions: not yet implemented\n");
		return 1;
	}

	fprintf(stderr, "kora: unknown command '%s'\n", argv[1]);
	usage();
	return 1;
}
