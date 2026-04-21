#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "client.h"
#include "config.h"
#include "db.h"
#include "dispatch.h"
#include "input.h"
#include "model.h"
#include "registry.h"
#include "server.h"
#include "status.h"
#include "tui.h"
#include "util.h"

/* --- daemon endpoint --- */

static const char *daemon_base_url(void)
{
	const char *env = getenv("KORA_BASE_URL");
	return (env && *env) ? env : "http://127.0.0.1:8818";
}

/* --- background model download (CLI-side, direct libcurl) --- */

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

/* --- shared cancel flag (gen + compact + naming) --- */

static volatile int op_cancel_flag = 0;

static void chunk_to_tui(const char *text, int len, void *user_data)
{
	(void)user_data;
	tui_assistant_chunk(text, len);
}

/* --- background generation --- */

static volatile int generating = 0;
static char *gen_response = NULL;

struct gen_args {
	char *base_url;
	char *model;
	struct kora_message *msgs;   /* deep-copied; thread frees */
	int n_msgs;
};

static void free_msgs(struct kora_message *msgs, int n)
{
	for (int i = 0; i < n; i++) {
		free((char *)msgs[i].role);
		free((char *)msgs[i].content);
	}
	free(msgs);
}

static struct kora_message *dup_msgs(const char **roles, const char **contents, int n)
{
	struct kora_message *msgs = calloc((size_t)n, sizeof(*msgs));
	if (!msgs) return NULL;
	for (int i = 0; i < n; i++) {
		msgs[i].role = strdup(roles[i]);
		msgs[i].content = strdup(contents[i]);
		if (!msgs[i].role || !msgs[i].content) {
			free_msgs(msgs, n);
			return NULL;
		}
	}
	return msgs;
}

static void *gen_thread_fn(void *arg)
{
	struct gen_args *ga = arg;

	tui_assistant_begin();

	struct kora_client_chat_opts opts = {
		.base_url = ga->base_url,
		.model    = ga->model,
		.msgs     = ga->msgs,
		.n_msgs   = ga->n_msgs,
		.chunk_cb = chunk_to_tui,
		.cancel   = &op_cancel_flag,
	};

	char *response = NULL;
	int rc = kora_client_chat(&opts, &response);

	/* if streaming produced nothing but the request failed, render the error
	   inline so the user sees something instead of an empty assistant block */
	if (rc != 0 && (!response || !*response)) {
		free(response);
		const char *err = "[error: chat request failed. is 'kora serve' running?]";
		tui_assistant_chunk(err, (int)strlen(err));
		response = strdup(err);
	}

	tui_assistant_end();
	tui_statusbar(NULL);
	gen_response = response;

	free_msgs(ga->msgs, ga->n_msgs);
	free(ga->base_url);
	free(ga->model);
	free(ga);
	generating = 0;
	return NULL;
}

static void gen_cancel(void)
{
	if (!generating) return;
	op_cancel_flag = 1;
	while (generating) {
		struct timespec ts = {0, 10 * 1000 * 1000};
		nanosleep(&ts, NULL);
	}
	op_cancel_flag = 0;
}

/* --- background compact --- */

static volatile int compacting = 0;
static char *compact_summary = NULL;
static int compact_before = 0;

struct compact_args {
	char *base_url;
	char *model;
	char *conversation;   /* "role: content\n..." */
	char *compact_prompt;
	int before_tokens;
};

static void *compact_thread_fn(void *arg)
{
	struct compact_args *ca = arg;

	tui_statusbar("compacting... (esc to cancel)");

	struct kora_message msgs[2] = {
		{ .role = "system", .content = ca->compact_prompt },
		{ .role = "user",   .content = ca->conversation },
	};
	struct kora_client_chat_opts opts = {
		.base_url = ca->base_url,
		.model    = ca->model,
		.msgs     = msgs,
		.n_msgs   = 2,
		.cancel   = &op_cancel_flag,
	};

	char *summary = NULL;
	int rc = kora_client_chat(&opts, &summary);
	tui_statusbar(NULL);

	if (rc == 0 && summary && *summary) {
		compact_summary = summary;
		compact_before = ca->before_tokens;
		int after = (int)(strlen(summary) / 4);  /* approx */
		char msg[128];
		snprintf(msg, sizeof msg, "Compacted: ~%d -> ~%d tokens",
		         ca->before_tokens, after);
		tui_info(""); tui_info(msg);
	} else {
		free(summary);
		tui_info(""); tui_info("Compaction failed.");
	}

	free(ca->base_url);
	free(ca->model);
	free(ca->conversation);
	free(ca->compact_prompt);
	free(ca);
	compacting = 0;
	return NULL;
}

static void compact_cancel(void)
{
	if (!compacting) return;
	op_cancel_flag = 1;
	while (compacting) {
		struct timespec ts = {0, 10 * 1000 * 1000};
		nanosleep(&ts, NULL);
	}
	op_cancel_flag = 0;
}

/* --- background session naming --- */

static volatile int naming_session = 0;
static char *session_name_result = NULL;
static int naming_session_id = -1;

struct naming_args {
	char *base_url;
	char *model;
	char *conversation;
	int session_id;
};

static void *naming_thread_fn(void *arg)
{
	struct naming_args *na = arg;
	tui_statusbar("naming session...");

	struct kora_message msgs[2] = {
		{ .role    = "system",
		  .content = "Generate a short title (3-5 words) for this conversation. "
		             "Reply with ONLY the title, no quotes, no punctuation at the end." },
		{ .role    = "user",
		  .content = na->conversation },
	};
	struct kora_client_chat_opts opts = {
		.base_url = na->base_url,
		.model    = na->model,
		.msgs     = msgs,
		.n_msgs   = 2,
	};

	char *name = NULL;
	int rc = kora_client_chat(&opts, &name);
	tui_statusbar(NULL);

	if (rc == 0 && name && *name) {
		char *end = name + strlen(name) - 1;
		while (end > name && (*end == '\n' || *end == '\r' || *end == ' '))
			*end-- = '\0';
		if (strlen(name) > 120) name[120] = '\0';
		session_name_result = name;
		naming_session_id = na->session_id;
	} else {
		free(name);
	}

	free(na->base_url);
	free(na->model);
	free(na->conversation);
	free(na);
	naming_session = 0;
	return NULL;
}

/* --- helpers --- */

static int parse_positive_int(const char *s)
{
	if (!s || !*s) return -1;
	for (const char *p = s; *p; p++)
		if (*p < '0' || *p > '9') return -1;
	int v = atoi(s);
	return v > 0 ? v : -1;
}

static int valid_model_name(const char *name)
{
	if (!name || !*name) return 0;
	if (strlen(name) > 128) return 0;
	for (const char *p = name; *p; p++) {
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		      (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.'))
			return 0;
	}
	return 1;
}

static int valid_pull_target(const char *target)
{
	if (!target || !*target) return 0;
	if (strlen(target) > 512) return 0;
	if (strncmp(target, "http://", 7) == 0 || strncmp(target, "https://", 8) == 0)
		return 1;
	return valid_model_name(target);
}

/* approximate total tokens across messages (chars/4). fast, local, no HTTP. */
static int approx_tokens(const char **roles, const char **contents, int n_msg)
{
	size_t bytes = 0;
	for (int i = 0; i < n_msg; i++)
		bytes += strlen(roles[i]) + strlen(contents[i]) + 8;  /* framing overhead */
	return (int)(bytes / 4);
}

static int ctx_needs_compression(int approx, int ctx_size)
{
	return ctx_size > 0 && approx > (int)(ctx_size * 0.75);
}

static void update_context_status(int approx_tokens_used, int ctx_size)
{
	if (ctx_size <= 0) {
		status_set(STATUS_CONTEXT, "/help for commands");
		return;
	}
	char buf[128];
	int pct = (int)((double)approx_tokens_used / ctx_size * 100);
	snprintf(buf, sizeof buf, "~%d/%d tokens (%d%%)",
	         approx_tokens_used, ctx_size, pct);
	status_set(STATUS_CONTEXT, buf);
}

static void usage(void)
{
	printf("Usage: kora <command> [args]\n"
	       "\n"
	       "Commands:\n"
	       "  chat [model]    Interactive chat\n"
	       "  pull <model>    Download a model\n"
	       "  list            List downloaded models\n"
	       "  rm <model>      Remove a model\n"
	       "  serve [model]   Start local inference server\n"
	       "  version         Print version\n");
}

static void version(void)
{
	printf("kora %s\n", VERSION);
}

/* resolve a model alias to a file path under ~/.kora/models/ */
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
	if (argc >= 2 && (strcmp(argv[1], "version") == 0 || strcmp(argv[1], "--version") == 0)) {
		version();
		return 0;
	}

	if (argc >= 2 && (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0)) {
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

	/* --- CLI-only commands --- */

	if (argc >= 2 && strcmp(argv[1], "pull") == 0) {
		if (argc < 3) {
			fprintf(stderr, "Usage: kora pull <model|url>\n\nAvailable models:\n");
			for (int i = 0; registry[i].alias; i++)
				fprintf(stderr, "  %-20s %s  %s\n",
					registry[i].alias, registry[i].size, registry[i].quant);
			fprintf(stderr, "\nOr provide a direct URL: kora pull https://...\n");
			db_close();
			return 1;
		}
		if (!valid_pull_target(argv[2])) {
			fprintf(stderr, "kora: invalid target '%s'\n", argv[2]);
			db_close();
			return 1;
		}
		int rc = model_pull(argv[2]);
		db_close();
		return rc != 0 ? 1 : 0;
	}

	if (argc >= 2 && strcmp(argv[1], "list") == 0) {
		model_list();
		db_close();
		return 0;
	}

	if (argc >= 2 && strcmp(argv[1], "rm") == 0) {
		if (argc < 3) {
			fprintf(stderr, "Usage: kora rm <model>\n");
			db_close();
			return 1;
		}
		if (!valid_model_name(argv[2])) {
			fprintf(stderr, "kora: invalid model name '%s'\n", argv[2]);
			db_close();
			return 1;
		}
		int rc = model_rm(argv[2]);
		db_close();
		return rc != 0 ? 1 : 0;
	}

	if (argc >= 2 && strcmp(argv[1], "serve") == 0) {
		struct kora_server_opts opts = {
			.model             = NULL,
			.public_port       = 8818,
			.ctx_size          = 8192,
			.idle_timeout_secs = 0,
			.pool_size         = 2,
			.parallel          = 1,
		};
		int i;

		for (i = 2; i < argc; i++) {
			if ((strcmp(argv[i], "--port") == 0 ||
			     strcmp(argv[i], "-p") == 0) && i + 1 < argc) {
				opts.public_port = atoi(argv[++i]);
			} else if ((strcmp(argv[i], "--model") == 0 ||
			            strcmp(argv[i], "-m") == 0) && i + 1 < argc) {
				opts.model = argv[++i];
			} else if ((strcmp(argv[i], "--ctx-size") == 0 ||
			            strcmp(argv[i], "-c") == 0) && i + 1 < argc) {
				opts.ctx_size = atoi(argv[++i]);
			} else if (strcmp(argv[i], "--idle-timeout") == 0 && i + 1 < argc) {
				opts.idle_timeout_secs = atoi(argv[++i]);
			} else if (strcmp(argv[i], "--pool-size") == 0 && i + 1 < argc) {
				opts.pool_size = atoi(argv[++i]);
			} else if (strcmp(argv[i], "--parallel") == 0 && i + 1 < argc) {
				opts.parallel = atoi(argv[++i]);
			} else if (argv[i][0] != '-') {
				opts.model = argv[i];
			}
		}

		char *preferred = NULL;
		if (!opts.model) {
			preferred = kora_preferred_model();
			opts.model = preferred;
		}
		if (!opts.model) {
			fprintf(stderr, "kora: no model specified\n"
				"Usage: kora serve [model] [--port PORT] "
				"[--ctx-size N] [--idle-timeout SECS] "
				"[--pool-size N] [--parallel N]\n");
			db_close();
			return 1;
		}

		db_close();
		int rc = kora_server_run(&opts);
		free(preferred);
		return rc;
	}

	if (argc < 2 || strcmp(argv[1], "chat") == 0) {
		struct kora_config *cfg = kora_config_load(LUADIR);

		/* resolve starting model: arg > -m > preferred > cfg->chat_model > cfg->default_model */
		const char *model_name = NULL;
		char *preferred = NULL;

		if (argc >= 3 && argv[2][0] != '-')
			model_name = argv[2];
		for (int i = 2; i < argc - 1; i++)
			if (strcmp(argv[i], "-m") == 0)
				model_name = argv[i + 1];
		if (!model_name) {
			preferred = kora_preferred_model();
			if (preferred) model_name = preferred;
		}
		if (!model_name && cfg->chat_model)    model_name = cfg->chat_model;
		if (!model_name && cfg->default_model) model_name = cfg->default_model;

		/* own the model name so it survives free(preferred) */
		char *current_model = model_name ? strdup(model_name) : NULL;
		free(preferred);
		preferred = NULL;

		const char *base_url = daemon_base_url();
		int daemon_up = (kora_client_ping(base_url) == 0);

		/* enter TUI */
		tui_init();
		status_wire(STATUS_CONTEXT, NULL, NULL, 0);
		tui_set_header("kora chat", current_model ? current_model : "no model");
		tui_draw();
		tui_draw_welcome(NULL);

		if (!daemon_up) {
			tui_info("");
			tui_info("Daemon not reachable at the configured base URL.");
			tui_info("Start it with: kora serve <model>  (or via systemd --user)");
			tui_info("");
		}

		/* session tracking */
		int session_id = -1;
		int msg_seq = 0;
		int user_msg_count = 0;
		int session_named = 0;

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
		}

		update_context_status(approx_tokens(roles, contents, n_msg), cfg->ctx_size);

		int running = 1;
		while (running) {
			char *input = tui_input("> ");

			/* collect completed background work */

			if (!generating && gen_response) {
				history_bufs[n_hist] = gen_response;
				roles[n_msg] = "assistant";
				contents[n_msg] = history_bufs[n_hist];
				n_msg++;
				n_hist++;

				if (session_id >= 0)
					db_message_add(session_id, msg_seq++, "assistant", gen_response, 1);

				gen_response = NULL;
				update_context_status(approx_tokens(roles, contents, n_msg), cfg->ctx_size);

				/* auto-name after 10 user messages */
				if (user_msg_count >= 10 && !session_named &&
				    !naming_session && session_id >= 0 && current_model) {
					char *conv_buf = malloc(8192);
					if (conv_buf) {
						int cpos = 0;
						for (int j = cfg->system_chat ? 1 : 0; j < n_msg && cpos < 7000; j++) {
							int len = snprintf(conv_buf + cpos, (size_t)(8192 - cpos),
								"%s: %s\n", roles[j], contents[j]);
							if (len > 0) cpos += len;
						}
						struct naming_args *na = malloc(sizeof(*na));
						pthread_t tid;
						na->base_url = strdup(base_url);
						na->model = strdup(current_model);
						na->conversation = conv_buf;
						na->session_id = session_id;
						naming_session = 1;
						pthread_create(&tid, NULL, naming_thread_fn, na);
						pthread_detach(tid);
					}
				}
			}

			if (!compacting && compact_summary) {
				for (int i = 0; i < n_hist; i++) free(history_bufs[i]);
				n_hist = 0;
				n_msg = cfg->system_chat ? 1 : 0;

				history_bufs[n_hist] = compact_summary;
				roles[n_msg] = "assistant";
				contents[n_msg] = history_bufs[n_hist];
				n_msg++;
				n_hist++;

				compact_summary = NULL;
				update_context_status(approx_tokens(roles, contents, n_msg), cfg->ctx_size);
			}

			if (!naming_session && session_name_result) {
				db_session_set_name(naming_session_id, session_name_result);
				session_named = 1;
				free(session_name_result);
				session_name_result = NULL;
			}

			if (!input) break;

			if (input[0] == '\0') {
				if (generating) {
					gen_cancel();
					free(gen_response);
					gen_response = NULL;
					tui_info(""); tui_info("Generation cancelled.");
				}
				if (compacting) {
					compact_cancel();
					free(compact_summary);
					compact_summary = NULL;
					tui_info(""); tui_info("Compaction cancelled.");
				}
				free(input);
				continue;
			}

			tui_user_msg(input);
			tui_input_clear();

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
				if (generating || compacting || naming_session) {
					tui_info("Busy. Press ESC to cancel current task first.");
				} else {
					const char *arg = input + 7;
					while (*arg == ' ') arg++;

					struct db_session sessions[20];
					int ns = db_sessions_list(sessions, 20);

					if (strncmp(arg, "rm ", 3) == 0) {
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
						tui_info("");
						tui_info("  #  Name                      Date           Msgs  Last message");
						tui_info("  ──────────────────────────────────────────────────────────────────");
						for (int i = 0; i < ns; i++) {
							char line[512];
							char name_trunc[25];
							snprintf(name_trunc, sizeof(name_trunc), "%s", sessions[i].name);

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
								i + 1, name_trunc, date_short,
								sessions[i].message_count, msg_trunc,
								strlen(sessions[i].last_message) > 30 ? "..." : "");
							tui_info(line);
						}
						tui_info("");
						tui_info("  /resume <#>       Resume a session");
						tui_info("  /resume rm <#>    Delete a session");
						tui_info("");
					} else {
						int sel = parse_positive_int(arg);
						if (sel < 1 || sel > ns) {
							tui_info("Invalid session number. Use /resume to see the list.");
						} else {
							int idx = sel - 1;
							int sid = sessions[idx].id;

							for (int i = 0; i < n_hist; i++) free(history_bufs[i]);
							n_hist = 0;
							n_msg = cfg->system_chat ? 1 : 0;

							struct db_message *msgs = NULL;
							int nm = db_messages_load(sid, &msgs);

							msg_seq = 0;
							user_msg_count = 0;
							for (int i = 0; i < nm && n_msg < 1 + MAX_TURNS * 2 - 1; i++) {
								if (!msgs[i].llm_use) continue;
								if (strcmp(msgs[i].role, "system") == 0) continue;
								history_bufs[n_hist] = strdup(msgs[i].content);
								roles[n_msg] = strcmp(msgs[i].role, "user") == 0 ? "user" : "assistant";
								contents[n_msg] = history_bufs[n_hist];
								n_msg++;
								n_hist++;
								if (strcmp(msgs[i].role, "user") == 0) user_msg_count++;
								if (msgs[i].seq > msg_seq) msg_seq = msgs[i].seq;
							}
							msg_seq++;
							db_messages_free(msgs, nm);

							session_id = sid;
							session_named = sessions[idx].name[0] != '\0' &&
								strcmp(sessions[idx].name, "New session") != 0;

							tui_draw();
							tui_draw_welcome(current_model);

							char resume_msg[256];
							snprintf(resume_msg, sizeof(resume_msg),
								"Resumed session: %s", sessions[idx].name);
							tui_info(resume_msg);
							tui_info("");

							for (int j = cfg->system_chat ? 1 : 0; j < n_msg; j++) {
								if (strcmp(roles[j], "user") == 0) {
									tui_user_msg(contents[j]);
								} else {
									tui_assistant_begin();
									tui_assistant_chunk(contents[j], 0);
									tui_assistant_end();
								}
							}

							update_context_status(approx_tokens(roles, contents, n_msg), cfg->ctx_size);
						}
					}
				}
				free(input);
			} else if (strcmp(input, "/clear") == 0) {
				for (int i = 0; i < n_hist; i++) free(history_bufs[i]);
				n_msg = cfg->system_chat ? 1 : 0;
				n_hist = 0;
				tui_draw();
				tui_draw_welcome(current_model);
				tui_info("Conversation cleared.");
				session_id = -1;
				msg_seq = 0;
				user_msg_count = 0;
				session_named = 0;
				update_context_status(approx_tokens(roles, contents, n_msg), cfg->ctx_size);
				free(input);
			} else if (strcmp(input, "/compact") == 0) {
				if (!current_model) {
					tui_info("No model selected. Use /model <name>.");
				} else if (generating || compacting || naming_session) {
					tui_info("Busy. Press ESC to cancel current task first.");
				} else if (n_msg <= 1) {
					tui_info("Nothing to compact.");
				} else {
					size_t cap = 16384;
					char *conv = malloc(cap);
					size_t pos = 0;
					for (int j = cfg->system_chat ? 1 : 0; j < n_msg; j++) {
						int need = (int)(strlen(roles[j]) + strlen(contents[j]) + 16);
						if (pos + (size_t)need >= cap) {
							cap *= 2;
							char *nb = realloc(conv, cap);
							if (!nb) break;
							conv = nb;
						}
						pos += (size_t)snprintf(conv + pos, cap - pos,
						                        "%s: %s\n", roles[j], contents[j]);
					}

					int before = (int)(pos / 4);

					struct compact_args *ca = malloc(sizeof(*ca));
					pthread_t tid;
					ca->base_url = strdup(base_url);
					ca->model = strdup(current_model);
					ca->conversation = conv;
					ca->compact_prompt = strdup(cfg->compact_chat ? cfg->compact_chat :
						"Summarize the following conversation concisely.");
					ca->before_tokens = before;
					compacting = 1;
					pthread_create(&tid, NULL, compact_thread_fn, ca);
					pthread_detach(tid);
				}
				free(input);
			} else if (strncmp(input, "/model", 6) == 0) {
				const char *new_name = input + 6;
				while (*new_name == ' ') new_name++;
				if (*new_name == '\0') {
					char model_msg[256];
					snprintf(model_msg, sizeof(model_msg), "Current model: %s",
						current_model ? current_model : "(none)");
					tui_info(model_msg);
					tui_info("");
					tui_info("  Models:");
					tui_info("  ──────────────────────────────────────────────────────────");
					db_models_each(tui_model_table_cb, NULL);
					tui_info("");
					tui_info("  /model <name>    Switch model (daemon loads on next request)");
					tui_info("  /pull <name>     Download a model");
					tui_info("");
				} else if (!valid_model_name(new_name)) {
					tui_info("Invalid model name. Use alphanumeric, hyphens, dots, underscores.");
				} else if (generating || compacting || naming_session) {
					tui_info("Busy. Press ESC to cancel current task first.");
				} else {
					char *new_path = resolve_model_path(new_name);
					if (!new_path) {
						tui_info("Could not resolve model.");
					} else if (!model_exists(new_path)) {
						tui_info("Model not found. Use /pull to download it.");
						free(new_path);
					} else {
						free(new_path);
						free(current_model);
						current_model = strdup(new_name);
						kora_set_preferred_model(current_model);
						db_model_set_active(current_model);
						tui_set_header("kora chat", current_model);

						for (int i = 0; i < n_hist; i++) free(history_bufs[i]);
						n_hist = 0;
						n_msg = cfg->system_chat ? 1 : 0;
						session_id = -1;
						msg_seq = 0;
						user_msg_count = 0;
						session_named = 0;

						char msg[256];
						snprintf(msg, sizeof msg,
						         "Switched to %s (daemon will load on next request).",
						         current_model);
						tui_info(msg);
						update_context_status(approx_tokens(roles, contents, n_msg), cfg->ctx_size);
					}
				}
				free(input);
			} else if (strncmp(input, "/pull", 5) == 0) {
				const char *target = input + 5;
				while (*target == ' ') target++;
				if (*target == '\0') {
					tui_info("Usage: /pull <model|url>");
				} else if (!valid_pull_target(target)) {
					tui_info("Invalid target. Use a model name or https:// URL.");
				} else if (dispatch_active()) {
					tui_info("A background task is already running.");
				} else {
					struct pull_args *pa = malloc(sizeof(*pa));
					snprintf(pa->target, sizeof(pa->target), "%s", target);
					char msg[256];
					snprintf(msg, sizeof msg, "Downloading %s in background...", target);
					tui_info(msg);
					dispatch(bg_pull_fn, pa);
				}
				free(input);
			} else if (input[0] == '/') {
				char err_msg[256];
				snprintf(err_msg, sizeof err_msg, "Unknown command: %s (type /help)", input);
				tui_info(err_msg);
				free(input);
			} else if (generating || compacting || naming_session) {
				tui_info("Busy. Press ESC to cancel.");
				free(input);
			} else if (!current_model) {
				tui_info("No model selected. Use /model <name> first.");
				free(input);
			} else if (n_msg >= 1 + MAX_TURNS * 2 - 1) {
				tui_info("Conversation too long, use /clear or /exit.");
				free(input);
				running = 0;
			} else {
				if (session_id < 0) {
					char cwd[512];
					if (!getcwd(cwd, sizeof(cwd))) cwd[0] = '\0';
					session_id = db_session_create("chat",
						current_model ? current_model : "", cwd);
				}

				history_bufs[n_hist] = strdup(input);
				roles[n_msg] = "user";
				contents[n_msg] = history_bufs[n_hist];
				n_msg++;
				n_hist++;
				user_msg_count++;

				if (session_id >= 0)
					db_message_add(session_id, msg_seq++, "user", input, 1);

				int tok = approx_tokens(roles, contents, n_msg);
				update_context_status(tok, cfg->ctx_size);

				/* auto-compact if approaching limit (best-effort; uses existing compact thread) */
				if (ctx_needs_compression(tok, cfg->ctx_size) && !compacting) {
					tui_info("Context approaching limit; compacting in background...");
					/* build conversation text */
					size_t cap = 16384;
					char *conv = malloc(cap);
					size_t pos = 0;
					for (int j = cfg->system_chat ? 1 : 0; j < n_msg; j++) {
						int need = (int)(strlen(roles[j]) + strlen(contents[j]) + 16);
						if (pos + (size_t)need >= cap) {
							cap *= 2;
							char *nb = realloc(conv, cap);
							if (!nb) break;
							conv = nb;
						}
						pos += (size_t)snprintf(conv + pos, cap - pos,
						                        "%s: %s\n", roles[j], contents[j]);
					}
					struct compact_args *ca = malloc(sizeof(*ca));
					pthread_t tid;
					ca->base_url = strdup(base_url);
					ca->model = strdup(current_model);
					ca->conversation = conv;
					ca->compact_prompt = strdup(cfg->compact_chat ? cfg->compact_chat :
						"Summarize the following conversation concisely.");
					ca->before_tokens = tok;
					compacting = 1;
					pthread_create(&tid, NULL, compact_thread_fn, ca);
					pthread_detach(tid);
				}

				struct gen_args *ga = malloc(sizeof(*ga));
				ga->base_url = strdup(base_url);
				ga->model = strdup(current_model);
				ga->msgs = dup_msgs(roles, contents, n_msg);
				ga->n_msgs = n_msg;
				if (!ga->msgs) {
					free(ga->base_url); free(ga->model); free(ga);
					tui_info("Out of memory.");
					free(input);
					continue;
				}
				gen_response = NULL;
				generating = 1;
				tui_statusbar("generating... (esc to cancel)");
				pthread_t tid;
				pthread_create(&tid, NULL, gen_thread_fn, ga);
				pthread_detach(tid);

				free(input);
			}
		}

		gen_cancel();
		compact_cancel();
		while (naming_session) {
			struct timespec ts = {0, 50 * 1000 * 1000};
			nanosleep(&ts, NULL);
		}

		free(gen_response);     gen_response = NULL;
		free(compact_summary);  compact_summary = NULL;
		free(session_name_result); session_name_result = NULL;

		for (int i = 0; i < n_hist; i++) free(history_bufs[i]);
		tui_cleanup();
		free(current_model);
		kora_config_free(cfg);
		db_close();
		return 0;
	}

	fprintf(stderr, "kora: unknown command '%s'\n", argv[1]);
	usage();
	return 1;
}
