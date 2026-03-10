#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "db.h"
#include "dispatch.h"
#include "inference.h"
#include "input.h"
#include "lua_bridge.h"
#include "model.h"
#include "registry.h"
#include "status.h"
#include "tui.h"
#include "util.h"

/* --- background model download --- */

static void bg_pull_progress_cb(int pct, double dl_mb, double total_mb, void *user_data)
{
	(void)user_data;
	char buf[128];
	snprintf(buf, sizeof(buf), "Downloading: %.0fMB / %.0fMB (%d%%)", dl_mb, total_mb, pct);
	status_set(STATUS_DOWNLOAD, buf);
}

struct pull_args {
	char target[256];
};

static void bg_pull_fn(void *arg)
{
	struct pull_args *pa = arg;

	status_wire(STATUS_DOWNLOAD, NULL, NULL, 0);
	model_set_progress_cb(bg_pull_progress_cb, NULL);
	int rc = model_pull(pa->target);
	model_set_progress_cb(NULL, NULL);
	status_unwire(STATUS_DOWNLOAD);

	if (rc == 0) {
		db_models_sync();
		tui_info("");
		tui_info("Download complete.");
	} else {
		tui_info("");
		tui_info("Download failed.");
	}

	free(pa);
}

/* callback for listing models in the TUI via db_models_each */
static void tui_model_table_cb(const char *alias, const char *filename,
                               const char *size, const char *quant,
                               int downloaded, int active, void *user_data)
{
	(void)filename;
	(void)user_data;
	char buf[256];
	snprintf(buf, sizeof(buf), "  %s%-18s  %5s  %-6s  %s",
		active ? "* " : "  ",
		alias,
		size ? size : "-",
		quant ? quant : "-",
		downloaded ? "downloaded" : "not downloaded");
	tui_info(buf);
}

/* streaming callback: sends tokens to the TUI chat window */
static void tui_stream_cb(const char *text, int len, void *user_data)
{
	(void)user_data;
	tui_assistant_chunk(text, len);
}

/* silent stream callback for background compact (discard output) */
static void null_stream_cb(const char *text, int len, void *user_data)
{
	(void)text; (void)len; (void)user_data;
}

/* --- background generation --- */

static volatile int generating = 0;
static char *gen_response = NULL;

struct gen_args {
	struct kora_ctx *kc;
	char *prompt;
};

static void *gen_thread_fn(void *arg)
{
	struct gen_args *ga = arg;

	tui_assistant_begin();
	kora_clear(ga->kc);
	kora_abort_reset();
	kora_generate(ga->kc, ga->prompt, &gen_response);
	tui_assistant_end();
	tui_statusbar(NULL);

	free(ga->prompt);
	free(ga);
	generating = 0;
	return NULL;
}

/* abort generation and wait for thread to finish */
static void gen_cancel(void)
{
	if (!generating)
		return;
	kora_abort();
	while (generating) {
		struct timespec ts = {0, 10000000};  /* 10ms */
		nanosleep(&ts, NULL);
	}
}

/* --- background compact --- */

static volatile int compacting = 0;
static char *compact_summary = NULL;
static int compact_before = 0;

struct compact_args {
	struct kora_ctx *kc;
	char *full_prompt;
	const char *compact_prompt;
	int before_tokens;
};

static void *compact_thread_fn(void *arg)
{
	struct compact_args *ca = arg;

	tui_statusbar("compacting... (esc to cancel)");
	kora_set_stream_cb(null_stream_cb, NULL);
	kora_abort_reset();
	char *summary = kora_summarize(ca->kc, ca->full_prompt, ca->compact_prompt);
	kora_set_stream_cb(tui_stream_cb, NULL);
	tui_statusbar(NULL);

	if (summary && summary[0] != '\0') {
		compact_summary = summary;
		compact_before = ca->before_tokens;

		int after = kora_token_count(ca->kc, summary);
		char compact_msg[128];
		snprintf(compact_msg, sizeof(compact_msg),
			"Compacted: %d -> %d tokens", ca->before_tokens, after);
		tui_info("");
		tui_info(compact_msg);
	} else {
		free(summary);
		tui_info("");
		tui_info("Compaction failed.");
	}

	free(ca->full_prompt);
	free(ca);
	compacting = 0;
	return NULL;
}

/* abort compact and wait for thread to finish */
static void compact_cancel(void)
{
	if (!compacting)
		return;
	kora_abort();
	while (compacting) {
		struct timespec ts = {0, 10000000};  /* 10ms */
		nanosleep(&ts, NULL);
	}
}

/* --- background model switch --- */

static volatile int switching_model = 0;
static struct kora_ctx *switch_result = NULL;
static char *switch_model_name = NULL;

struct switch_args {
	char *model_name;
	char *model_path;
	int ctx_size;
	int gpu_layers;
	int threads;
};

static void *switch_thread_fn(void *arg)
{
	struct switch_args *sa = arg;

	tui_statusbar("loading model...");
	int saved_err = kora_stderr_suppress();
	struct kora_ctx *new_kc = kora_load(sa->model_path, sa->ctx_size,
	                                    sa->gpu_layers, sa->threads);
	kora_stderr_restore(saved_err);
	tui_statusbar(NULL);

	if (new_kc) {
		switch_result = new_kc;
		switch_model_name = sa->model_name;  /* transfer ownership */
		char loaded_msg[256];
		snprintf(loaded_msg, sizeof(loaded_msg),
			 "Loaded %s", sa->model_name);
		tui_info("");
		tui_info(loaded_msg);
	} else {
		tui_info("");
		tui_info("Failed to load model.");
		free(sa->model_name);
	}

	free(sa->model_path);
	free(sa);
	switching_model = 0;
	return NULL;
}

/* --- background session naming --- */

static volatile int naming_session = 0;
static char *session_name_result = NULL;
static int naming_session_id = -1;

struct naming_args {
	struct kora_ctx *kc;
	char *conversation;
	int session_id;
};

static void *naming_thread_fn(void *arg)
{
	struct naming_args *na = arg;

	tui_statusbar("naming session...");
	kora_set_stream_cb(null_stream_cb, NULL);
	kora_abort_reset();

	const char *roles[2] = { "system", "user" };
	const char *contents[2] = {
		"Generate a short title (3-5 words) for this conversation. "
		"Reply with ONLY the title, no quotes, no punctuation at the end.",
		na->conversation
	};

	char *prompt = kora_apply_template(na->kc, roles, contents, 2);
	if (prompt) {
		kora_clear(na->kc);
		char *name = NULL;
		int ret = kora_generate(na->kc, prompt, &name);
		free(prompt);

		if (ret > 0 && name && name[0] != '\0') {
			/* trim whitespace and newlines */
			char *end = name + strlen(name) - 1;
			while (end > name && (*end == '\n' || *end == '\r' || *end == ' '))
				*end-- = '\0';

			/* truncate to 120 chars */
			if (strlen(name) > 120)
				name[120] = '\0';

			session_name_result = name;
			naming_session_id = na->session_id;
		} else {
			free(name);
		}
	}

	kora_set_stream_cb(tui_stream_cb, NULL);
	tui_statusbar(NULL);
	free(na->conversation);
	free(na);
	naming_session = 0;
	return NULL;
}

/* validate that a string is a positive integer, returns the value or -1 */
static int parse_positive_int(const char *s)
{
	if (!s || !*s)
		return -1;
	const char *p = s;
	while (*p) {
		if (*p < '0' || *p > '9')
			return -1;
		p++;
	}
	int val = atoi(s);
	return val > 0 ? val : -1;
}

/* validate a model name or alias: alphanumeric, hyphens, dots, underscores */
static int valid_model_name(const char *name)
{
	if (!name || !*name)
		return 0;
	if (strlen(name) > 128)
		return 0;
	const char *p = name;
	while (*p) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		    (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
		    *p == '.')
			p++;
		else
			return 0;
	}
	return 1;
}

/* validate a pull target: model name or URL */
static int valid_pull_target(const char *target)
{
	if (!target || !*target)
		return 0;
	if (strlen(target) > 512)
		return 0;
	/* URLs are fine — libcurl handles them safely */
	if (strncmp(target, "http://", 7) == 0 || strncmp(target, "https://", 8) == 0)
		return 1;
	/* otherwise must be a valid model name */
	return valid_model_name(target);
}

static void update_context_status(struct kora_ctx *kc, const char **roles,
                                  const char **contents, int n_msg)
{
	char buf[128];
	char *full_prompt;
	int used, pct;

	if (!kc) {
		status_set(STATUS_CONTEXT, "/help for commands");
		return;
	}

	full_prompt = kora_apply_template(kc, roles, contents, n_msg);
	used = full_prompt ? kora_token_count(kc, full_prompt) : 0;
	free(full_prompt);

	pct = kc->n_ctx > 0 ? (int)((double)used / kc->n_ctx * 100) : 0;
	snprintf(buf, sizeof(buf), "%d/%d tokens (%d%%)", used, kc->n_ctx, pct);
	status_set(STATUS_CONTEXT, buf);
}

static void usage(void)
{
	printf("Usage: kora <command> [args]\n"
	       "\n"
	       "Commands:\n"
	       "  chat [model]    Interactive chat\n"
	       "  code [model]    Agentic coding assistant\n"
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
		return 0;
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

	if (db_open() != 0) {
		fprintf(stderr, "kora: failed to open database\n");
		return 1;
	}
	db_models_sync();

	if (strcmp(argv[1], "chat") == 0) {
		/* load config */
		struct kora_config *cfg = kora_config_load(LUADIR);
		struct kora_ctx *kc = NULL;
		char *current_model = NULL;
		const char *model_name = NULL;
		char *preferred = NULL;
		int i;

		/* resolve model: arg > -m flag > preferred_model */
		if (argc >= 3 && argv[2][0] != '-')
			model_name = argv[2];

		for (i = 2; i < argc - 1; i++) {
			if (strcmp(argv[i], "-m") == 0)
				model_name = argv[i + 1];
		}

		if (!model_name) {
			preferred = kora_preferred_model();
			if (preferred)
				model_name = preferred;
		}

		kora_suppress_logs();

		/* resolve model path before entering TUI */
		char *model_path = NULL;
		int model_found = 0;

		if (model_name) {
			model_path = resolve_model_path(model_name);
			if (model_path && model_exists(model_path))
				model_found = 1;
		}

		/* enter TUI */
		tui_init();
		status_wire(STATUS_CONTEXT, NULL, NULL, 0);
		tui_set_header("kora chat", model_found ? model_name : "no model");
		tui_draw();
		tui_draw_welcome(NULL);

		/* launch async model load if we have a valid path */
		if (model_found) {
			struct switch_args *sa = malloc(sizeof(*sa));
			pthread_t switch_tid;
			sa->model_name = strdup(model_name);
			sa->model_path = model_path;  /* thread frees */
			sa->ctx_size = cfg->ctx_size;
			sa->gpu_layers = cfg->gpu_layers;
			sa->threads = cfg->threads;
			switching_model = 1;
			tui_info("");
			tui_info("Loading model...");
			pthread_create(&switch_tid, NULL, switch_thread_fn, sa);
			pthread_detach(switch_tid);
		} else {
			free(model_path);
			if (model_name) {
				char msg[256];
				snprintf(msg, sizeof(msg), "Model '%s' not found.", model_name);
				tui_info(msg);
			}
			tui_info("No model loaded. Use /model to see available models, /pull to download one.");
		}
		free(preferred);
		preferred = NULL;

		/* session tracking */
		int session_id = -1;
		int msg_seq = 0;
		int user_msg_count = 0;
		int session_named = 0;

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

		update_context_status(kc, roles, contents, n_msg);
		int running = 1;
		while (running) {
			char *input = tui_input("> ");

			/* collect completed background tasks — must run after
			   tui_input returns so results that arrive while the
			   user was typing are picked up before processing input */

			/* collect completed generation */
			if (!generating && gen_response) {
				history_bufs[n_hist] = gen_response;
				roles[n_msg] = "assistant";
				contents[n_msg] = history_bufs[n_hist];
				n_msg++;
				n_hist++;

				/* save assistant message to session */
				if (session_id >= 0) {
					db_message_add(session_id, msg_seq++,
						"assistant", gen_response, 1);
				}

				gen_response = NULL;
				update_context_status(kc, roles, contents, n_msg);

				/* auto-name session after 10 user messages.
				   triggered here (after generation completes) so
				   the naming thread doesn't compete with generation
				   for the inference context */
				if (user_msg_count >= 10 && !session_named &&
				    !naming_session && session_id >= 0 && kc) {
					char *conv_buf = malloc(8192);
					if (conv_buf) {
						int cpos = 0;
						int j;
						for (j = cfg->system_chat ? 1 : 0; j < n_msg && cpos < 7000; j++) {
							int len = snprintf(conv_buf + cpos, 8192 - cpos,
								"%s: %s\n", roles[j], contents[j]);
							if (len > 0) cpos += len;
						}

						struct naming_args *na = malloc(sizeof(*na));
						pthread_t naming_tid;
						na->kc = kc;
						na->conversation = conv_buf;
						na->session_id = session_id;
						naming_session = 1;
						pthread_create(&naming_tid, NULL, naming_thread_fn, na);
						pthread_detach(naming_tid);
					}
				}
			}

			/* collect completed compact */
			if (!compacting && compact_summary) {
				for (i = 0; i < n_hist; i++)
					free(history_bufs[i]);
				n_hist = 0;
				n_msg = cfg->system_chat ? 1 : 0;

				history_bufs[n_hist] = compact_summary;
				roles[n_msg] = "assistant";
				contents[n_msg] = history_bufs[n_hist];
				n_msg++;
				n_hist++;

				compact_summary = NULL;
				update_context_status(kc, roles, contents, n_msg);
			}

			/* collect completed model switch */
			if (!switching_model && switch_result) {
				if (kc) kora_free(kc);
				kc = switch_result;
				switch_result = NULL;
				kora_set_stream_cb(tui_stream_cb, NULL);

				free(current_model);
				current_model = switch_model_name;
				model_name = current_model;
				switch_model_name = NULL;

				kora_set_preferred_model(model_name);
				db_model_set_active(model_name);
				tui_set_header("kora chat", model_name);

				/* reset conversation */
				for (i = 0; i < n_hist; i++)
					free(history_bufs[i]);
				n_hist = 0;
				n_msg = cfg->system_chat ? 1 : 0;

				update_context_status(kc, roles, contents, n_msg);
			}

			/* collect completed session naming */
			if (!naming_session && session_name_result) {
				db_session_set_name(naming_session_id, session_name_result);
				session_named = 1;
				free(session_name_result);
				session_name_result = NULL;
			}
			if (!input)
				break;
			if (input[0] == '\0') {
				if (generating) {
					gen_cancel();
					free(gen_response);
					gen_response = NULL;
					tui_info("");
					tui_info("Generation cancelled.");
				}
				if (compacting) {
					compact_cancel();
					free(compact_summary);
					compact_summary = NULL;
					tui_info("");
					tui_info("Compaction cancelled.");
				}
				free(input);
				continue;
			}

			/* show input in chat */
			tui_user_msg(input);
			tui_input_clear();

			/* slash commands */
			if (strcmp(input, "/exit") == 0 || strcmp(input, "exit") == 0 ||
			    strcmp(input, "/quit") == 0 || strcmp(input, "quit") == 0) {
				gen_cancel();
				compact_cancel();
				free(input);
				running = 0;
			} else if (strcmp(input, "/help") == 0) {
				tui_info("/help           Show this message");
				tui_info("/model <name>   Switch to a different model");
				tui_info("/pull <name>    Download a model");
				tui_info("/compact        Summarize conversation to free context");
				tui_info("/clear          Clear conversation history");
				tui_info("/resume         Resume or manage previous sessions");
				tui_info("/exit           Quit chat");
				free(input);
			} else if (strncmp(input, "/resume", 7) == 0) {
				if (generating || compacting || switching_model || naming_session) {
					tui_info("Busy. Press ESC to cancel current task first.");
				} else {
					const char *arg = input + 7;
					while (*arg == ' ') arg++;

					struct db_session sessions[20];
					int ns = db_sessions_list(sessions, 20);

					if (strncmp(arg, "rm ", 3) == 0) {
						/* /resume rm <#> — delete a session by list number */
						int del = parse_positive_int(arg + 3);
						if (del < 1 || del > ns) {
							tui_info("Invalid session number. Use /resume to see the list.");
						} else {
							int del_id = sessions[del - 1].id;
							if (del_id == session_id) {
								session_id = -1;
								msg_seq = 0;
								user_msg_count = 0;
								session_named = 0;
							}
							db_session_delete(del_id);
							tui_info("Session deleted.");
						}
					} else if (ns == 0) {
						tui_info("No previous sessions.");
					} else if (*arg == '\0') {
						/* bare /resume — show the list */
						tui_info("");
						tui_info("  #  Name                      Date           Msgs  Last message");
						tui_info("  ──────────────────────────────────────────────────────────────────");
						for (i = 0; i < ns; i++) {
							char line[512];
							char name_trunc[25];
							snprintf(name_trunc, sizeof(name_trunc), "%s", sessions[i].name);

							/* shorten date: "2026-03-08 14:23:45" -> "Mar 08 14:23" */
							char date_short[16];
							const char *months[] = {
								"Jan","Feb","Mar","Apr","May","Jun",
								"Jul","Aug","Sep","Oct","Nov","Dec"
							};
							int yr, mo, dy, hh, mm;
							if (sscanf(sessions[i].updated_at, "%d-%d-%d %d:%d",
								   &yr, &mo, &dy, &hh, &mm) == 5 && mo >= 1 && mo <= 12) {
								snprintf(date_short, sizeof(date_short),
									"%s %02d %02d:%02d", months[mo-1], dy, hh, mm);
							} else {
								snprintf(date_short, sizeof(date_short), "%.15s",
									sessions[i].updated_at);
							}

							char msg_trunc[31];
							snprintf(msg_trunc, sizeof(msg_trunc), "%s", sessions[i].last_message);
							char *nl;
							while ((nl = strchr(msg_trunc, '\n')) != NULL) *nl = ' ';

							snprintf(line, sizeof(line), "  %-2d %-25s %-14s %4d  %s%s",
								i + 1, name_trunc,
								date_short,
								sessions[i].message_count,
								msg_trunc,
								strlen(sessions[i].last_message) > 30 ? "..." : "");
							tui_info(line);
						}
						tui_info("");
						tui_info("  /resume <#>       Resume a session");
						tui_info("  /resume rm <#>    Delete a session");
						tui_info("");
					} else {
						/* /resume <n> — select a session */
						int sel = parse_positive_int(arg);
						if (sel < 1 || sel > ns) {
							tui_info("Invalid session number. Use /resume to see the list.");
						} else {
							int idx = sel - 1;
							int sid = sessions[idx].id;

							/* clear current conversation */
							for (i = 0; i < n_hist; i++)
								free(history_bufs[i]);
							n_hist = 0;
							n_msg = cfg->system_chat ? 1 : 0;
							if (kc) kora_clear(kc);

							/* load session messages */
							struct db_message *msgs = NULL;
							int nm = db_messages_load(sid, &msgs);

							msg_seq = 0;
							user_msg_count = 0;
							for (i = 0; i < nm && n_msg < 1 + MAX_TURNS * 2 - 1; i++) {
								if (!msgs[i].llm_use)
									continue;
								if (strcmp(msgs[i].role, "system") == 0)
									continue;
								history_bufs[n_hist] = strdup(msgs[i].content);
								roles[n_msg] = strcmp(msgs[i].role, "user") == 0 ? "user" : "assistant";
								contents[n_msg] = history_bufs[n_hist];
								n_msg++;
								n_hist++;
								if (strcmp(msgs[i].role, "user") == 0)
									user_msg_count++;
								if (msgs[i].seq > msg_seq)
									msg_seq = msgs[i].seq;
							}
							msg_seq++;
							db_messages_free(msgs, nm);

							session_id = sid;
							session_named = sessions[idx].name[0] != '\0' &&
								strcmp(sessions[idx].name, "New session") != 0;

							/* redraw and replay messages into the TUI */
							tui_draw();
							tui_draw_welcome(kc ? model_name : NULL);

							char resume_msg[256];
							snprintf(resume_msg, sizeof(resume_msg),
								"Resumed session: %s",
								sessions[idx].name);
							tui_info(resume_msg);
							tui_info("");

							/* replay conversation into chat window */
							for (int j = cfg->system_chat ? 1 : 0; j < n_msg; j++) {
								if (strcmp(roles[j], "user") == 0) {
									tui_user_msg(contents[j]);
								} else {
									tui_assistant_begin();
									tui_assistant_chunk(contents[j], 0);
									tui_assistant_end();
								}
							}

							update_context_status(kc, roles, contents, n_msg);
						}
					}
				}
				free(input);
			} else if (strcmp(input, "/clear") == 0) {
				for (i = 0; i < n_hist; i++)
					free(history_bufs[i]);
				n_msg = cfg->system_chat ? 1 : 0;
				n_hist = 0;
				if (kc) kora_clear(kc);
				tui_draw();
				tui_draw_welcome(kc ? model_name : NULL);
				tui_info("Conversation cleared.");

				/* start a fresh session */
				session_id = -1;
				msg_seq = 0;
				user_msg_count = 0;
				session_named = 0;

				update_context_status(kc, roles, contents, n_msg);
				free(input);
			} else if (strcmp(input, "/compact") == 0) {
				if (!kc) {
					tui_info("No model loaded.");
				} else if (generating || compacting || switching_model || naming_session) {
					tui_info("Busy. Press ESC to cancel current task first.");
				} else if (n_msg <= 1) {
					tui_info("Nothing to compact.");
				} else {
					char *full_prompt = kora_apply_template(kc, roles, contents, n_msg);
					if (full_prompt) {
						int before = kora_token_count(kc, full_prompt);

						struct compact_args *ca = malloc(sizeof(*ca));
						pthread_t compact_tid;
						ca->kc = kc;
						ca->full_prompt = full_prompt;  /* thread frees */
						ca->compact_prompt = cfg->compact_chat;
						ca->before_tokens = before;
						compacting = 1;
						pthread_create(&compact_tid, NULL, compact_thread_fn, ca);
						pthread_detach(compact_tid);
					}
				}
				free(input);
			} else if (strncmp(input, "/model", 6) == 0) {
				const char *new_name = input + 6;
				while (*new_name == ' ')
					new_name++;
				if (*new_name == '\0') {
					char model_msg[256];
					snprintf(model_msg, sizeof(model_msg), "Current model: %s", model_name);
					tui_info(model_msg);
					tui_info("");
					tui_info("  Models:");
					tui_info("  ──────────────────────────────────────────────────────────");
					db_models_each(tui_model_table_cb, NULL);
					tui_info("");
					tui_info("  /model <name>              Switch model");
					tui_info("  /pull <name|url>           Download a model");
					tui_info("");
				} else if (!valid_model_name(new_name)) {
					tui_info("Invalid model name. Use alphanumeric, hyphens, dots, underscores.");
				} else if (generating || compacting || switching_model || naming_session) {
					tui_info("Busy. Press ESC to cancel current task first.");
				} else {
					char *new_path = resolve_model_path(new_name);
					if (!new_path) {
						tui_info("Could not resolve model.");
					} else if (!model_exists(new_path)) {
						tui_info("Model not found. Use /pull to download it.");
						free(new_path);
					} else {
						struct switch_args *sa = malloc(sizeof(*sa));
						pthread_t switch_tid;
						sa->model_name = strdup(new_name);
						sa->model_path = new_path;  /* thread frees */
						sa->ctx_size = cfg->ctx_size;
						sa->gpu_layers = cfg->gpu_layers;
						sa->threads = cfg->threads;
						switching_model = 1;
						tui_info("Loading model...");
						pthread_create(&switch_tid, NULL, switch_thread_fn, sa);
						pthread_detach(switch_tid);
					}
				}
				free(input);
			} else if (strncmp(input, "/pull", 5) == 0) {
				const char *target = input + 5;
				while (*target == ' ')
					target++;
				if (*target == '\0') {
					tui_info("Usage: /pull <model|url>");
				} else if (!valid_pull_target(target)) {
					tui_info("Invalid target. Use a model name or https:// URL.");
				} else if (dispatch_active()) {
					tui_info("A background task is already running.");
				} else {
					struct pull_args *pa = malloc(sizeof(*pa));
					snprintf(pa->target, sizeof(pa->target), "%s", target);

					char pull_msg[256];
					snprintf(pull_msg, sizeof(pull_msg), "Downloading %s in background...", target);
					tui_info(pull_msg);
					dispatch(bg_pull_fn, pa);
				}
				free(input);
			} else if (input[0] == '/') {
				char err_msg[256];
				snprintf(err_msg, sizeof(err_msg), "Unknown command: %s (type /help)", input);
				tui_info(err_msg);
				free(input);
			} else if (generating || compacting || switching_model || naming_session) {
				tui_info("Busy. Press ESC to cancel.");
				free(input);
			} else if (!kc) {
				tui_info("No model loaded. Use /model to see models, /pull to download.");
				free(input);
			} else if (n_msg >= 1 + MAX_TURNS * 2 - 1) {
				tui_info("Conversation too long, use /clear or /exit.");
				free(input);
				running = 0;
			} else {
				/* create session on first user message */
				if (session_id < 0) {
					char cwd[512];
					if (!getcwd(cwd, sizeof(cwd)))
						cwd[0] = '\0';
					session_id = db_session_create("chat",
						model_name ? model_name : "", cwd);
				}

				/* add user message */
				history_bufs[n_hist] = strdup(input);
				roles[n_msg] = "user";
				contents[n_msg] = history_bufs[n_hist];
				n_msg++;
				n_hist++;
				user_msg_count++;

				/* save user message to session */
				if (session_id >= 0)
					db_message_add(session_id, msg_seq++, "user", input, 1);

				update_context_status(kc, roles, contents, n_msg);
				/* apply template with full conversation */
				char *prompt = kora_apply_template(kc, roles, contents, n_msg);
				if (!prompt) {
					tui_info("Failed to apply chat template.");
					n_msg--;
					n_hist--;
					free(history_bufs[--n_hist]);
					free(input);
					continue;
				}

				/* compress context if approaching the limit */
				int prompt_tokens = kora_token_count(kc, prompt);
				if (kora_context_needs_compression(kc, prompt_tokens)) {
					tui_info("Compressing context...");
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
						tui_info("Failed to apply template after compression.");
						free(input);
						continue;
					}
				}

				/* generate response in background */
				{
					struct gen_args *ga = malloc(sizeof(*ga));
					pthread_t gen_tid;
					ga->kc = kc;
					ga->prompt = prompt;  /* gen_thread_fn frees this */
					gen_response = NULL;
					generating = 1;
					tui_statusbar("generating... (esc to cancel)");
					pthread_create(&gen_tid, NULL, gen_thread_fn, ga);
					pthread_detach(gen_tid);
				}

				free(input);
			}
		}

		gen_cancel();
		compact_cancel();
		/* kora_load is not abortable — wait for switch to finish */
		while (switching_model) {
			struct timespec ts = {0, 50000000};  /* 50ms */
			nanosleep(&ts, NULL);
		}
		/* wait for naming to finish */
		while (naming_session) {
			struct timespec ts = {0, 50000000};
			nanosleep(&ts, NULL);
		}

		free(gen_response);
		gen_response = NULL;
		free(compact_summary);
		compact_summary = NULL;
		free(session_name_result);
		session_name_result = NULL;
		if (switch_result) {
			kora_free(switch_result);
			switch_result = NULL;
		}
		free(switch_model_name);
		switch_model_name = NULL;

		for (i = 0; i < n_hist; i++)
			free(history_bufs[i]);
		tui_cleanup();
		free(current_model);
		if (kc) kora_free(kc);
		kora_config_free(cfg);
		db_close();
		return 0;
	}

	if (strcmp(argv[1], "code") == 0) {
		printf("Code: not yet implemented\n");
		return 1;
	}

	fprintf(stderr, "kora: unknown command '%s'\n", argv[1]);
	usage();
	return 1;
}
