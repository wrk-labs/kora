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
#include "prompt.h"
#include "registry.h"
#include "server.h"
#include "session.h"
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

/* number of in-flight background downloads. bumped before dispatch, decremented
   when bg_pull_fn exits. main thread reads it to decide whether to warn on
   quit. download_cancel is polled by model.c's progress callback — setting
   it to 1 aborts curl and triggers the .part unlink path. */
static volatile int downloads_in_flight = 0;
static volatile int download_cancel = 0;

static void bg_pull_fn(void *arg)
{
	struct pull_args *pa = arg;

	status_wire(STATUS_DOWNLOAD, NULL, NULL, 0);
	model_set_progress_cb(bg_pull_progress_cb, NULL);
	model_set_cancel_flag(&download_cancel);
	/* publish the in-flight alias so the MODELS pane can render the
	   ⟳ glyph on the matching row. URL pulls won't match any row, which
	   is fine — the indicator just won't show. */
	tui_set_downloading_alias(pa->target);
	int rc = model_pull(pa->target);
	tui_set_downloading_alias(NULL);
	model_set_cancel_flag(NULL);
	model_set_progress_cb(NULL, NULL);
	status_unwire(STATUS_DOWNLOAD);

	if (rc == 0) {
		db_models_sync();
		tui_log("Download complete: %s", pa->target);
		tui_post_models_refresh();   /* thread-safe; main loop redraws */
	} else if (download_cancel) {
		tui_log("Download cancelled: %s", pa->target);
		tui_post_models_refresh();
	} else {
		tui_log("Download failed: %s", pa->target);
		tui_post_models_refresh();
	}

	/* clear the cancel flag so the next pull isn't aborted on the first
	   progress tick. safe to reset here unconditionally: we've already
	   branched on its value above, and dispatch_active() guarantees no
	   overlapping worker is racing to read it. */
	__atomic_store_n(&download_cancel, 0, __ATOMIC_SEQ_CST);
	__atomic_sub_fetch(&downloads_in_flight, 1, __ATOMIC_SEQ_CST);
	free(pa);
}

static void tui_model_table_cb(const char *alias, const char *filename,
                               const char *size, const char *quant,
                               int downloaded, int active,
                               const char *display_name, void *user_data)
{
	(void)filename;
	(void)display_name;
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
/* set by gen_thread_fn when dispatch fails (e.g. daemon down). main loop
   reads this to roll back the trailing user message from the in-memory
   session so the next request stays well-formed. -1 = no failure pending. */
static volatile int gen_failed_user_seq = -1;

struct gen_args {
	char *base_url;
	char *model;
	struct kora_message *msgs;   /* snapshot from kora_session_snapshot */
	int n_msgs;
	/* persistence: save the assistant reply to the DB the moment
	   generation finishes, rather than deferring to the next main-loop
	   iteration (which only runs when the user presses Enter again). */
	int save_session_id;         /* -1 to skip */
	int save_seq;
};

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

	if (rc != 0 && (!response || !*response)) {
		free(response);
		const char *err = "[error: chat request failed. is 'kora serve' running?]";
		tui_assistant_chunk(err, (int)strlen(err));
		response = NULL;   /* don't persist an error as an assistant turn */

		/* roll back the user message we already wrote in the main loop:
		   mark it 'failed' in the DB so it's not replayed in future
		   context, and signal the main thread to drop it from the
		   in-memory session (so the next request stays well-formed). */
		if (ga->save_session_id >= 0)
			db_message_set_status(ga->save_session_id,
			                      ga->save_seq - 1, "failed");
		gen_failed_user_seq = ga->save_seq - 1;
	}

	tui_assistant_end();
	tui_statusbar(NULL);

	/* persist the assistant response immediately. sqlite runs in its
	   default serialized mode, so db_message_add is thread-safe. this
	   ensures the reply survives a Ctrl-C right after reading it — the
	   old path only saved when the user typed their next message. */
	if (rc == 0 && response && *response &&
	    ga->save_session_id >= 0)
		db_message_add(ga->save_session_id, ga->save_seq,
		               "assistant", response, ga->model, 1);

	gen_response = response;

	kora_session_snapshot_free(ga->msgs, ga->n_msgs);
	free(ga->base_url);
	free(ga->model);
	free(ga);
	generating = 0;
	tui_set_cancellable(0);
	/* let the main loop know a turn just completed so any queued follow-up
	   message can be fired automatically without the user pressing a key. */
	tui_post_wake();
	return NULL;
}

/* shared cancel loop: set the flag, poll the op's running-bit until the
   worker notices and clears it, then reset so the next op isn't pre-cancelled.
   generating/compacting are volatile, so the compiler won't hoist the load. */
static void cancel_op(volatile int *running)
{
	if (!*running) return;
	op_cancel_flag = 1;
	while (*running) {
		struct timespec ts = {0, 10 * 1000 * 1000};
		nanosleep(&ts, NULL);
	}
	op_cancel_flag = 0;
}

static void gen_cancel(void)     { cancel_op(&generating); }

/* --- background compact --- */

static volatile int compacting = 0;
static char *compact_summary = NULL;
static int compact_before = 0;

struct compact_args {
	char *base_url;
	char *model;
	char *conversation;
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
		int after = (int)(strlen(summary) / 4);
		tui_log("Compacted: ~%d → ~%d tokens", ca->before_tokens, after);
	} else {
		free(summary);
		tui_log("Compaction failed.");
	}

	free(ca->base_url);
	free(ca->model);
	free(ca->conversation);
	free(ca->compact_prompt);
	free(ca);
	compacting = 0;
	tui_set_cancellable(0);
	return NULL;
}

static void compact_cancel(void) { cancel_op(&compacting); }

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

static int ctx_needs_compression(int tokens, int ctx_size)
{
	return ctx_size > 0 && tokens > (int)(ctx_size * 0.75);
}

static void update_context_status(const struct kora_session *session, int ctx_size)
{
	int tokens = kora_session_approx_tokens(session);
	if (ctx_size <= 0) {
		status_set(STATUS_CONTEXT, NULL);
		return;
	}
	char buf[128];
	int pct = (int)((double)tokens / ctx_size * 100);
	snprintf(buf, sizeof buf, "~%d/%d tokens (%d%%)", tokens, ctx_size, pct);
	status_set(STATUS_CONTEXT, buf);
}

/* append a short system-role event marking a mid-conversation model swap.
   the startup system prompt is left alone — this is just a signal in the
   transcript so the model (and anyone reading DB history) can see the swap
   happened. */
static void notice_model_switch(struct kora_session *s,
                                const char *from, const char *to)
{
	if (!s || !to || !*to) return;
	char msg[256];
	if (from && *from)
		snprintf(msg, sizeof msg, "[Model changed from `%s` to `%s`.]", from, to);
	else
		snprintf(msg, sizeof msg, "[Model changed to `%s`.]", to);
	kora_session_add(s, "system", msg);
}

/* refresh the left pane with the latest session list from the DB. */
static void refresh_sessions_pane(int active_id)
{
	struct db_session list[64];
	int n = db_sessions_list(list, 64);
	tui_set_sessions(list, n, active_id);
}

/* --- daemon-status background poller --- */

static volatile int g_poller_stop = 0;

struct poller_args {
	char base_url[256];
};

static void *daemon_poller_fn(void *arg)
{
	struct poller_args *pa = arg;
	int last = -1;
	while (!g_poller_stop) {
		int up = (kora_client_ping(pa->base_url) == 0);
		if (up != last) {
			tui_post_daemon_status(up);
			/* skip the initial startup "transition" so we don't
			   double-up with the "Daemon unreachable…" line the
			   chat-branch prints right after init. */
			if (last >= 0) tui_log(up ? "Daemon up." : "Daemon down.");
			last = up;
		}
		/* sleep 2s in 100ms chunks so shutdown is responsive */
		for (int i = 0; i < 20 && !g_poller_stop; i++) {
			struct timespec ts = { 0, 100 * 1000 * 1000 };
			nanosleep(&ts, NULL);
		}
	}
	free(pa);
	return NULL;
}

/* populate the right admin pane with registry models + downloaded flags.
   downloaded (locally available) models are listed first so the user sees
   what they can actually use without scrolling. */
struct manual_probe_ctx {
	struct tui_model_row *rows;
	int *n;
	int cap;
	const char *current_model;
};

static void manual_models_cb(const char *alias, const char *filename,
                             const char *size, const char *quant,
                             int downloaded, int active,
                             const char *display_name, void *user_data)
{
	(void)size; (void)quant; (void)active;
	struct manual_probe_ctx *ctx = user_data;
	if (!downloaded) return;
	if (*ctx->n >= ctx->cap) return;
	/* skip anything already emitted by the registry passes */
	for (int i = 0; i < *ctx->n; i++) {
		if (strcmp(ctx->rows[i].alias, alias) == 0) return;
		if (filename && strcmp(ctx->rows[i].alias, filename) == 0) return;
	}
	snprintf(ctx->rows[*ctx->n].alias, sizeof ctx->rows[*ctx->n].alias, "%s", alias);
	if (display_name && *display_name)
		snprintf(ctx->rows[*ctx->n].display,
		         sizeof ctx->rows[*ctx->n].display, "%s", display_name);
	else
		ctx->rows[*ctx->n].display[0] = '\0';
	ctx->rows[*ctx->n].size[0]  = '\0';   /* unknown for manual */
	ctx->rows[*ctx->n].quant[0] = '\0';
	ctx->rows[*ctx->n].downloaded = 1;
	ctx->rows[*ctx->n].is_current = (ctx->current_model &&
	                                 strcmp(ctx->current_model, alias) == 0);
	(*ctx->n)++;
}

static void refresh_models_pane(const char *current_model)
{
	struct tui_model_row rows[32];
	memset(rows, 0, sizeof rows);   /* clears display[] so rows without
	                                   a GGUF name fall back to alias */
	int n = 0;

	/* pass 1: downloaded registry models */
	for (int i = 0; registry[i].alias && n < 32; i++) {
		if (!db_model_is_downloaded(registry[i].alias)) continue;
		snprintf(rows[n].alias, sizeof rows[n].alias, "%s", registry[i].alias);
		snprintf(rows[n].size,  sizeof rows[n].size,  "%s", registry[i].size  ? registry[i].size  : "");
		snprintf(rows[n].quant, sizeof rows[n].quant, "%s", registry[i].quant ? registry[i].quant : "");
		rows[n].downloaded = 1;
		rows[n].is_current = (current_model &&
		                      strcmp(current_model, registry[i].alias) == 0);
		n++;
	}

	/* pass 2: manually-downloaded (non-registry) models — anything that
	   ended up in ~/.kora/models/ via a URL pull. db_models_sync has
	   already added them to the DB with source='manual'. */
	struct manual_probe_ctx ctx = {
		.rows = rows, .n = &n, .cap = 32, .current_model = current_model,
	};
	db_models_each(manual_models_cb, &ctx);

	/* pass 3: not-downloaded registry models (fetchable) */
	for (int i = 0; registry[i].alias && n < 32; i++) {
		if (db_model_is_downloaded(registry[i].alias)) continue;
		snprintf(rows[n].alias, sizeof rows[n].alias, "%s", registry[i].alias);
		snprintf(rows[n].size,  sizeof rows[n].size,  "%s", registry[i].size  ? registry[i].size  : "");
		snprintf(rows[n].quant, sizeof rows[n].quant, "%s", registry[i].quant ? registry[i].quant : "");
		rows[n].downloaded = 0;
		rows[n].is_current = 0;
		n++;
	}

	tui_set_models(rows, n);
}

/* refresh callback invoked by the TUI main loop when bg_pull_fn finishes.
   reads current model from db so we don't need a thread-shared pointer. */
static void refresh_models_pane_auto(void)
{
	const char *active = db_model_get_active();
	refresh_models_pane(active);
}

/* update the header's displayed session name from the DB (if active). */
static void refresh_session_header(const struct kora_session *s)
{
	if (!s || s->db_id < 0) {
		tui_set_session_name("");
		return;
	}
	struct db_session row;
	if (db_session_get(s->db_id, &row) == 0 && row.name[0] &&
	    strcmp(row.name, "New session") != 0) {
		tui_set_session_name(row.name);
	} else {
		tui_set_session_name("(new)");
	}
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

static char *resolve_model_path(const char *model_name)
{
	char *path = model_resolve_path(model_name);
	if (!path)
		fprintf(stderr, "kora: invalid model name '%s'\n", model_name);
	return path;
}

/* --- launch helpers: build args + spawn thread for gen/compact/naming --- */

static int launch_gen(const char *base_url, const char *model,
                      struct kora_session *session)
{
	struct kora_message *msgs = NULL;
	int n = kora_session_snapshot(session, &msgs);
	if (n < 0) return -1;

	struct gen_args *ga = malloc(sizeof *ga);
	if (!ga) { kora_session_snapshot_free(msgs, n); return -1; }
	ga->base_url = strdup(base_url);
	ga->model    = strdup(model);
	ga->msgs     = msgs;
	ga->n_msgs   = n;
	/* reserve the DB seq now so the worker can save without racing
	   on session->msg_seq. the main loop's collection path then skips
	   the DB call since it's already persisted. */
	ga->save_session_id = session->db_id;
	ga->save_seq        = session->msg_seq++;

	gen_response = NULL;
	generating = 1;
	tui_set_cancellable(1);
	tui_statusbar("generating... (esc to cancel)");
	pthread_t tid;
	pthread_create(&tid, NULL, gen_thread_fn, ga);
	pthread_detach(tid);
	return 0;
}

static int launch_compact(const char *base_url, const char *model,
                          const struct kora_session *session,
                          const char *compact_prompt,
                          int before_tokens)
{
	char *conv = kora_session_transcript(session);
	if (!conv) return -1;

	struct compact_args *ca = malloc(sizeof *ca);
	if (!ca) { free(conv); return -1; }
	ca->base_url       = strdup(base_url);
	ca->model          = strdup(model);
	ca->conversation   = conv;
	ca->compact_prompt = strdup(compact_prompt ? compact_prompt
	                      : "Summarize the following conversation concisely.");
	ca->before_tokens  = before_tokens;

	compacting = 1;
	tui_set_cancellable(1);
	pthread_t tid;
	pthread_create(&tid, NULL, compact_thread_fn, ca);
	pthread_detach(tid);
	return 0;
}

static int launch_naming(const char *base_url, const char *model,
                         const struct kora_session *session,
                         int session_id)
{
	char *conv = kora_session_transcript(session);
	if (!conv) return -1;

	struct naming_args *na = malloc(sizeof *na);
	if (!na) { free(conv); return -1; }
	na->base_url     = strdup(base_url);
	na->model        = strdup(model);
	na->conversation = conv;
	na->session_id   = session_id;

	naming_session = 1;
	pthread_t tid;
	pthread_create(&tid, NULL, naming_thread_fn, na);
	pthread_detach(tid);
	return 0;
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
		const char *model_arg = NULL;
		char *preferred = NULL;

		if (argc >= 3 && argv[2][0] != '-')
			model_arg = argv[2];
		for (int i = 2; i < argc - 1; i++)
			if (strcmp(argv[i], "-m") == 0)
				model_arg = argv[i + 1];
		if (!model_arg) {
			preferred = kora_preferred_model();
			if (preferred) model_arg = preferred;
		}
		if (!model_arg && cfg->chat_model)    model_arg = cfg->chat_model;
		if (!model_arg && cfg->default_model) model_arg = cfg->default_model;

		char *current_model = model_arg ? strdup(model_arg) : NULL;
		free(preferred);
		preferred = NULL;

		const char *base_url = daemon_base_url();
		int daemon_up = (kora_client_ping(base_url) == 0);

		/* session holds the active conversation state. render the
		   system-prompt template with the current env (date/time/model/
		   ctx/platform) before installing it on the session. */
		char *initial_sys = kora_prompt_render(cfg->system_chat,
		                                       current_model, cfg->ctx_size);
		struct kora_session *session =
			kora_session_new(current_model, initial_sys);
		free(initial_sys);
		if (!session) {
			fprintf(stderr, "kora: out of memory\n");
			free(current_model);
			kora_config_free(cfg);
			db_close();
			return 1;
		}

		tui_init();
		tui_set_markdown(cfg->markdown);
		status_wire(STATUS_CONTEXT, NULL, NULL, 0);
		tui_set_header("kora", current_model ? current_model : "no model");
		tui_set_daemon_status(daemon_up);
		tui_set_session_name("");
		tui_draw();
		tui_draw_welcome(NULL);
		refresh_sessions_pane(session->db_id);
		refresh_models_pane(current_model);
		tui_set_models_refresh_cb(refresh_models_pane_auto);

		/* background poller: ping daemon every 2s, push status to TUI.
		   this is how the header dot reflects "daemon came up later". */
		pthread_t poller_tid;
		struct poller_args *pa = malloc(sizeof *pa);
		snprintf(pa->base_url, sizeof pa->base_url, "%s", base_url);
		g_poller_stop = 0;
		pthread_create(&poller_tid, NULL, daemon_poller_fn, pa);

		if (!daemon_up) {
			tui_log("Daemon unreachable at %s — run: kora serve <model>", base_url);
		}

		update_context_status(session, cfg->ctx_size);

		/* unbounded FIFO of pending user messages. if the user submits a
		   chat turn while a generation is running, we append to this
		   list instead of discarding with a "Busy" message. the bg thread
		   calls tui_post_wake() when its turn ends; main.c sees the wake
		   sentinel, pops the head of the queue, echoes it to chat, and
		   runs the normal send path. Esc-cancel clears the entire queue
		   — the user cancelled, they probably don't want a chain of
		   surprise messages to fire after a deliberate abort. */
		struct pending_msg { char *text; struct pending_msg *next; };
		struct pending_msg *queue_head = NULL;
		struct pending_msg *queue_tail = NULL;
		int queue_count = 0;

		/* refresh the statusbar indicator to reflect the current queue.
		   the preview always shows the head (the one that'll fire next)
		   truncated to keep the bottom bar compact. a leading count is
		   prepended when more than one message is pending. */
		#define QUEUE_REFRESH_INDICATOR() do { \
			if (queue_count == 0) { \
				tui_set_queued(NULL); \
			} else { \
				char _ind[120]; \
				int  _full = (int)strlen(queue_head->text); \
				int  _plen = _full > 40 ? 40 : _full; \
				const char *_ellip = _full > 40 ? "…" : ""; \
				if (queue_count == 1) \
					snprintf(_ind, sizeof _ind, "↑ queued: %.*s%s", \
					         _plen, queue_head->text, _ellip); \
				else \
					snprintf(_ind, sizeof _ind, "↑ %d queued: %.*s%s", \
					         queue_count, _plen, queue_head->text, _ellip); \
				tui_set_queued(_ind); \
			} \
		} while (0)

		int running = 1;
		while (running) {
			char *input = tui_input("> ");

			/* collect completed background work */

			if (!generating && gen_response) {
				/* DB write already happened in gen_thread_fn — here we
				   only mirror the response into the in-memory session
				   so the next turn and the rendered pad stay in sync. */
				kora_session_add(session, "assistant", gen_response);
				free(gen_response);
				gen_response = NULL;
				update_context_status(session, cfg->ctx_size);

				if (session->user_msg_count >= 3 && !session->named &&
				    !naming_session && session->db_id >= 0 && current_model)
					launch_naming(base_url, current_model, session, session->db_id);
			}

			if (!generating && gen_failed_user_seq >= 0) {
				/* dispatch failed: drop the trailing user message from
				   the in-memory session so the next request doesn't
				   replay an unanswered turn. the DB row stays around
				   tagged 'failed' for audit / future retry UI. */
				if (session->n_msg > 0 &&
				    strcmp(session->roles[session->n_msg - 1], "user") == 0) {
					free(session->roles[session->n_msg - 1]);
					free(session->contents[session->n_msg - 1]);
					session->n_msg--;
					if (session->user_msg_count > 0)
						session->user_msg_count--;
					update_context_status(session, cfg->ctx_size);
				}
				gen_failed_user_seq = -1;
				tui_log("Message not delivered (daemon unreachable). Retype to retry.");
			}

			if (!compacting && compact_summary) {
				kora_session_clear(session);
				kora_session_add(session, "assistant", compact_summary);
				free(compact_summary);
				compact_summary = NULL;
				update_context_status(session, cfg->ctx_size);
			}

			if (!naming_session && session_name_result) {
				db_session_set_name(naming_session_id, session_name_result);
				session->named = 1;
				free(session_name_result);
				session_name_result = NULL;
				refresh_sessions_pane(session->db_id);
				refresh_session_header(session);
			}

			if (!input) break;

			/* wake sentinel: bg worker finished and wants the main loop
			   to act. pop the head of the pending queue and fire it
			   now that the channel is free. */
			if (input[0] == '\x02') {
				free(input);
				if (queue_head && !generating && !compacting &&
				    !naming_session && current_model) {
					struct pending_msg *node = queue_head;
					queue_head = node->next;
					if (!queue_head) queue_tail = NULL;
					queue_count--;
					char *msg = node->text;
					free(node);
					QUEUE_REFRESH_INDICATOR();
					/* synthesise the same fall-through the direct-send
					   path takes: echo to chat, then goto the chat-send
					   block with `input` set to the queued text. */
					tui_user_msg(msg);
					input = msg;
					goto chat_send_path;
				}
				continue;
			}

			if (input[0] == '\0') {
				if (generating) {
					gen_cancel();
					free(gen_response);
					gen_response = NULL;
					tui_log("Generation cancelled.");
				}
				if (compacting) {
					compact_cancel();
					free(compact_summary);
					compact_summary = NULL;
					tui_log("Compaction cancelled.");
				}
				/* the user cancelled — assume they also want to abandon
				   every queued follow-up. retyping is cheaper than having
				   a chain of surprise messages fire after a deliberate
				   cancel. */
				while (queue_head) {
					struct pending_msg *node = queue_head;
					queue_head = node->next;
					free(node->text);
					free(node);
				}
				queue_tail = NULL;
				queue_count = 0;
				tui_set_queued(NULL);
				free(input);
				continue;
			}

			tui_input_clear();

			/* commands come from NORMAL-mode shortcuts, which encode them
			   with a leading \x01 (SOH) marker. user-typed "/anything" in
			   INSERT mode never has that byte, so it's a plain chat
			   message. normalise the marker to '/' so the existing
			   command-dispatch chain keeps working unchanged. */
			int is_cmd = ((unsigned char)input[0] == 1);
			if (is_cmd) input[0] = '/';

			/* echo chat messages into the chat pad — but only when we'll
			   actually send them now. if we're busy (generation in flight)
			   the message goes to the queue instead of the chat pad, and
			   chat_send_path's busy branch handles the echo-once-sent
			   bookkeeping via tui_set_queued(). */
			if (!is_cmd && !generating && !compacting && !naming_session)
				tui_user_msg(input);

			if (!is_cmd) {
				/* not a command — skip the slash-dispatch chain entirely
				   and fall straight through to the chat-send branch.
				   typed slashes are just message content. */
				goto chat_send_path;
			}

			if (strcmp(input, "/exit") == 0 ||
			    strcmp(input, "/quit") == 0) {
				int n_dl = __atomic_load_n(&downloads_in_flight, __ATOMIC_SEQ_CST);
				if (n_dl > 0) {
					char prompt[128];
					snprintf(prompt, sizeof prompt,
					         "Download in progress — cancel and quit?");
					int ans = tui_permission(prompt);
					if (ans != 'y') {
						/* user declined; keep running */
						free(input);
						continue;
					}
					/* signal the download thread to abort; spin-wait until
					   it unwinds so the .part file gets unlinked before
					   the process exits. */
					__atomic_store_n(&download_cancel, 1, __ATOMIC_SEQ_CST);
					tui_log("Cancelling download…");
					while (__atomic_load_n(&downloads_in_flight,
					                        __ATOMIC_SEQ_CST) > 0) {
						struct timespec ts = {0, 50 * 1000 * 1000};
						nanosleep(&ts, NULL);
					}
					__atomic_store_n(&download_cancel, 0, __ATOMIC_SEQ_CST);
				}
				gen_cancel();
				compact_cancel();
				free(input);
				running = 0;
			} else if (strcmp(input, "/help") == 0) {
				/* silently swallow help during any background op so a
				   stray Alt+H mid-stream doesn't splice text into the
				   assistant's output. */
				if (generating || compacting || naming_session) {
					free(input);
					continue;
				}
				/* debounce: prevent help spam */
				static time_t last_help_time = 0;
				time_t now = time(NULL);
				if (now - last_help_time < 2) {
					free(input);
					continue;
				}
				last_help_time = now;
				/* one tui_info call → one leading/trailing blank for the
				   whole block, rather than one per row. render_info
				   re-indents each embedded newline to match. */
				tui_info(
					"────────────────────────────────────────\n"
					"Help:\n"
					"\n"
					"Global:\n"
					"  Tab            Cycle focus forward (SESSIONS → CHAT → MODELS)\n"
					"  Shift+Tab      Cycle focus backward\n"
					"  Esc            Cancel: generation, or command entry (r/m/p)\n"
					"  Ctrl-C         Quit kora\n"
					"\n"
					"CHAT:\n"
					"  type + Enter   Send message\n"
					"  Alt+Enter      Insert newline\n"
					"  Alt+H          Show help\n"
					"\n"
					"SESSIONS:\n"
					"  j / k          Highlight next / previous session\n"
					"  Enter          Open highlighted session\n"
					"  n              New session\n"
					"  d              Delete highlighted session\n"
					"  r              Rename highlighted session (type the new name)\n"
					"\n"
					"MODELS:\n"
					"  j / k          Highlight next / previous model\n"
					"  Enter          Switch to highlighted model\n"
					"  p              Pull the highlighted model\n"
					"  u              Pull a model by URL (prompts)\n"
					"  d              Remove the highlighted model (or cancel its download)\n"
					"\n"
					"Note: context auto-compacts when > 75% full. No manual command.\n"
					"────────────────────────────────────────");
				free(input);
			} else if (strcmp(input, "/open") == 0) {
				/* open the session highlighted in the SESSIONS pane */
				if (generating || compacting || naming_session) {
					tui_info("Busy. Press Esc to cancel current task first.");
				} else {
					int sid = tui_highlighted_session_id();
					if (sid < 0) {
						tui_info("No session highlighted.");
					} else if (sid == session->db_id) {
						/* already open — just focus CHAT */
						tui_focus_chat();
					} else {
						kora_session_clear(session);
						struct db_message *msgs = NULL;
						int nm = db_messages_load_for_context(sid, &msgs);
						session->msg_seq = 0;
						session->user_msg_count = 0;
						for (int i = 0; i < nm; i++) {
							if (!msgs[i].llm_use) continue;
							if (strcmp(msgs[i].role, "system") == 0) continue;
							kora_session_add(session, msgs[i].role, msgs[i].content);
							if (strcmp(msgs[i].role, "user") == 0)
								session->user_msg_count++;
							if (msgs[i].seq > session->msg_seq)
								session->msg_seq = msgs[i].seq;
						}
						session->msg_seq++;
						db_messages_free(msgs, nm);

						struct db_session info;
						int named = 0;
						if (db_session_get(sid, &info) == 0)
							named = info.name[0] != '\0' &&
							        strcmp(info.name, "New session") != 0;
						session->db_id = sid;
						session->named = named;

						refresh_sessions_pane(session->db_id);
						refresh_session_header(session);
						tui_draw();
						tui_draw_welcome(current_model);
						for (int j = 0; j < session->n_msg; j++) {
							if (strcmp(session->roles[j], "system") == 0) continue;
							if (strcmp(session->roles[j], "user") == 0) {
								tui_user_msg(session->contents[j]);
							} else {
								tui_assistant_begin();
								tui_assistant_chunk(session->contents[j], 0);
								tui_assistant_end();
							}
						}
						update_context_status(session, cfg->ctx_size);
						tui_focus_chat();
					}
				}
				free(input);
			} else if (strcmp(input, "/model_switch") == 0) {
				const char *alias = tui_highlighted_model_alias();
				if (!alias) {
					tui_info("No model highlighted.");
				} else if (!valid_model_name(alias)) {
					tui_info("Invalid model name.");
				} else if (generating || compacting || naming_session) {
					tui_info("Busy. Press Esc to cancel current task first.");
				} else {
					char *new_path = resolve_model_path(alias);
					if (!new_path || !model_exists(new_path)) {
						tui_log("Model not downloaded: %s. Press 'p' in MODELS pane to fetch it.", alias);
						free(new_path);
					} else {
						free(new_path);
						/* capture the outgoing alias before we overwrite it
						   — the in-history swap notice wants both sides. */
						char *old_model = current_model;
						current_model = strdup(alias);
						/* in-session swap: keep the transcript and DB row;
						   subsequent turns get tagged with the new model on
						   save, and the daemon routes to the right child via
						   the `model` field in each request. */
						kora_session_set_model(session, current_model);
						kora_set_preferred_model(current_model);
						db_model_set_active(current_model);
						tui_set_header("kora", current_model);

						/* leave the top-level system prompt alone; append a
						   short system-role event so the transcript (and the
						   model) sees the swap. */
						notice_model_switch(session, old_model, current_model);
						free(old_model);

						tui_log("Switched to %s (continues current session).",
						        current_model);
						refresh_models_pane(current_model);
						refresh_session_header(session);
						update_context_status(session, cfg->ctx_size);
						tui_focus_chat();
					}
				}
				free(input);
			} else if (strcmp(input, "/model_pull") == 0) {
				const char *alias = tui_highlighted_model_alias();
				if (!alias) {
					tui_info("No model highlighted.");
				} else if (dispatch_active()) {
					tui_info("A background task is already running.");
				} else {
					struct pull_args *pa = malloc(sizeof *pa);
					if (!pa) {
						tui_info("Out of memory.");
					} else {
						snprintf(pa->target, sizeof pa->target, "%s", alias);
						tui_log("Downloading %s…", alias);
						__atomic_add_fetch(&downloads_in_flight, 1, __ATOMIC_SEQ_CST);
						dispatch(bg_pull_fn, pa);
					}
				}
				free(input);
			} else if (strcmp(input, "/model_cancel") == 0) {
				/* fired when the user presses 'x' on a row that's
				   currently being pulled. flags the worker; curl's
				   progress callback returns 1 on the next tick and the
				   .part file gets unlinked in model.c. we don't wait
				   here — the pane refresh happens when bg_pull_fn
				   exits and posts tui_post_models_refresh(). */
				int n_dl = __atomic_load_n(&downloads_in_flight, __ATOMIC_SEQ_CST);
				if (n_dl <= 0) {
					tui_info("Nothing is downloading.");
				} else {
					__atomic_store_n(&download_cancel, 1, __ATOMIC_SEQ_CST);
					tui_log("Cancelling download…");
				}
				free(input);
			} else if (strcmp(input, "/model_rm") == 0) {
				const char *alias = tui_highlighted_model_alias();
				if (!alias) {
					tui_info("No model highlighted.");
				} else if (!valid_model_name(alias)) {
					tui_info("Invalid model name.");
				} else if (current_model && strcmp(alias, current_model) == 0) {
					tui_info("Can't remove the active model. Switch first.");
				} else if (!db_model_is_downloaded(alias)) {
					tui_log("Model not downloaded: %s", alias);
				} else {
					int rc = model_rm(alias);
					if (rc == 0) {
						tui_log("Removed %s from disk.", alias);
						db_models_sync();
						refresh_models_pane(current_model);
					} else {
						tui_log("Remove failed: %s", alias);
					}
					tui_input_clear();
				}
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
							if (del_id == session->db_id) {
								session->db_id = -1;
								session->msg_seq = 0;
								session->user_msg_count = 0;
								session->named = 0;
								tui_clear_chat();
							}
							db_session_delete(del_id);
							tui_log("Session deleted.");
							tui_input_clear();
						}
					} else if (ns == 0) {
						tui_info("No previous sessions.");
					} else if (*arg == '\0') {
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
						tui_info("  /resume <#>       Resume a session");
						tui_info("  /resume rm <#>    Delete a session");
					} else {
						int sel = parse_positive_int(arg);
						if (sel < 1 || sel > ns) {
							tui_info("Invalid session number. Use /resume to see the list.");
						} else {
							int idx = sel - 1;
							int sid = sessions[idx].id;

							kora_session_clear(session);

							struct db_message *msgs = NULL;
							int nm = db_messages_load_for_context(sid, &msgs);

							session->msg_seq = 0;
							session->user_msg_count = 0;
							for (int i = 0; i < nm; i++) {
								if (!msgs[i].llm_use) continue;
								if (strcmp(msgs[i].role, "system") == 0) continue;
								kora_session_add(session, msgs[i].role, msgs[i].content);
								if (strcmp(msgs[i].role, "user") == 0)
									session->user_msg_count++;
								if (msgs[i].seq > session->msg_seq)
									session->msg_seq = msgs[i].seq;
							}
							session->msg_seq++;
							db_messages_free(msgs, nm);

							session->db_id = sid;
							session->named = sessions[idx].name[0] != '\0' &&
								strcmp(sessions[idx].name, "New session") != 0;

							refresh_sessions_pane(session->db_id);
							refresh_session_header(session);
							tui_draw();
							tui_draw_welcome(current_model);

							tui_log("Resumed session: %s", sessions[idx].name);
							tui_input_clear();

							for (int j = 0; j < session->n_msg; j++) {
								if (strcmp(session->roles[j], "system") == 0) continue;
								if (strcmp(session->roles[j], "user") == 0) {
									tui_user_msg(session->contents[j]);
								} else {
									tui_assistant_begin();
									tui_assistant_chunk(session->contents[j], 0);
									tui_assistant_end();
								}
							}

							update_context_status(session, cfg->ctx_size);
						}
					}
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
					tui_info("  Models:");
					tui_info("  ──────────────────────────────────────────────────────────");
					db_models_each(tui_model_table_cb, NULL);
					tui_info("  /model <name>    Switch model (daemon loads on next request)");
					tui_info("  /pull <name>     Download a model");
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
						kora_session_set_model(session, current_model);
						kora_set_preferred_model(current_model);
						db_model_set_active(current_model);
						tui_set_header("kora", current_model);

						kora_session_clear(session);
						session->db_id = -1;
						session->msg_seq = 0;
						session->user_msg_count = 0;
						session->named = 0;

						tui_log("Switched to %s (daemon will load on next request).",
						        current_model);
						refresh_models_pane(current_model);
						refresh_sessions_pane(-1);
						refresh_session_header(session);
						update_context_status(session, cfg->ctx_size);
					}
				}
				free(input);
			} else if (strcmp(input, "/next") == 0 ||
			           strcmp(input, "/prev") == 0) {
				int forward = (strcmp(input, "/next") == 0);
				if (generating || compacting || naming_session) {
					tui_info("Busy. Press ESC to cancel current task first.");
				} else {
					struct db_session sl[64];
					int ns = db_sessions_list(sl, 64);
					if (ns == 0) {
						tui_info("No sessions to switch to.");
					} else {
						/* find current session's index; if not found (new/unsaved),
						   default to just past the end so next→0 / prev→last */
						int cur_idx = -1;
						for (int i = 0; i < ns; i++)
							if (sl[i].id == session->db_id) { cur_idx = i; break; }

						int target;
						if (cur_idx < 0) {
							target = forward ? 0 : ns - 1;
						} else {
							target = forward ? (cur_idx + 1) % ns
							                 : (cur_idx - 1 + ns) % ns;
						}

						int sid = sl[target].id;
						if (sid == session->db_id) {
							tui_info("Only one session.");
						} else {
							/* reuse /resume N code path */
							kora_session_clear(session);
							struct db_message *msgs = NULL;
							int nm = db_messages_load_for_context(sid, &msgs);
							session->msg_seq = 0;
							session->user_msg_count = 0;
							for (int i = 0; i < nm; i++) {
								if (!msgs[i].llm_use) continue;
								if (strcmp(msgs[i].role, "system") == 0) continue;
								kora_session_add(session,
								                 msgs[i].role, msgs[i].content);
								if (strcmp(msgs[i].role, "user") == 0)
									session->user_msg_count++;
								if (msgs[i].seq > session->msg_seq)
									session->msg_seq = msgs[i].seq;
							}
							session->msg_seq++;
							db_messages_free(msgs, nm);

							session->db_id = sid;
							session->named = sl[target].name[0] != '\0' &&
								strcmp(sl[target].name, "New session") != 0;

							refresh_sessions_pane(session->db_id);
							refresh_session_header(session);
							tui_draw();
							tui_draw_welcome(current_model);
							tui_log("Switched to: %s",
							        sl[target].name[0] ? sl[target].name : "New session");
							for (int j = 0; j < session->n_msg; j++) {
								if (strcmp(session->roles[j], "system") == 0) continue;
								if (strcmp(session->roles[j], "user") == 0) {
									tui_user_msg(session->contents[j]);
								} else {
									tui_assistant_begin();
									tui_assistant_chunk(session->contents[j], 0);
									tui_assistant_end();
								}
							}
							update_context_status(session, cfg->ctx_size);
						}
					}
				}
				free(input);
			} else if (strcmp(input, "/new") == 0) {
				if (generating || compacting || naming_session) {
					tui_info("Busy. Press ESC to cancel current task first.");
				} else {
					kora_session_clear(session);
					session->db_id = -1;
					session->msg_seq = 0;
					session->user_msg_count = 0;
					session->named = 0;
					tui_draw();
					tui_draw_welcome(current_model);
					tui_log("New session.");
					refresh_sessions_pane(-1);
					refresh_session_header(session);
					update_context_status(session, cfg->ctx_size);
					tui_input_clear();
				}
				free(input);
			} else if (strncmp(input, "/rename", 7) == 0) {
				const char *name = input + 7;
				while (*name == ' ') name++;
				int target_id = tui_highlighted_session_id();
				if (*name == '\0') {
					tui_info("Usage: highlight a session and type the new name.");
				} else if (target_id < 0) {
					tui_info("No session highlighted.");
				} else if (strlen(name) > 120) {
					tui_info("Name too long (max 120 chars).");
				} else {
					db_session_set_name(target_id, name);
					if (target_id == session->db_id)
						session->named = 1;
					tui_log("Renamed to: %s", name);
					refresh_sessions_pane(session->db_id);
					refresh_session_header(session);
					tui_input_clear();
				}
				free(input);
			} else if (strcmp(input, "/delete") == 0) {
				int del_id = tui_highlighted_session_id();
				if (del_id < 0) {
					tui_info("No session highlighted.");
				} else if (generating || compacting || naming_session) {
					tui_info("Busy. Press Esc to cancel current task first.");
				} else {
					db_session_delete(del_id);
					/* if we deleted the session currently open in the
					   chat pane, reset its state */
					if (del_id == session->db_id) {
						kora_session_clear(session);
						session->db_id = -1;
						session->msg_seq = 0;
						session->user_msg_count = 0;
						session->named = 0;
						tui_clear_chat();
						tui_draw_welcome(current_model);
					}
					tui_log("Session deleted.");
					refresh_sessions_pane(session->db_id);
					refresh_session_header(session);
					update_context_status(session, cfg->ctx_size);
					tui_input_clear();
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
					if (!pa) {
						tui_info("Out of memory.");
					} else {
						snprintf(pa->target, sizeof(pa->target), "%s", target);
						tui_log("Downloading %s…", target);
						__atomic_add_fetch(&downloads_in_flight, 1, __ATOMIC_SEQ_CST);
						dispatch(bg_pull_fn, pa);
					}
				}
				free(input);
			} else {
				/* unknown command (marker was set but we didn't recognise it) */
				char err_msg[256];
				snprintf(err_msg, sizeof err_msg, "Unknown command: %s", input);
				tui_info(err_msg);
				free(input);
				continue;
			}
			continue;  /* command branch handled; skip chat path */

chat_send_path:
			if (generating || compacting || naming_session) {
				/* append to the FIFO. the head fires first when the
				   current op finishes; subsequent entries fire as each
				   generation completes. */
				struct pending_msg *node = malloc(sizeof *node);
				char *text_copy = node ? strdup(input) : NULL;
				if (node && text_copy) {
					node->text = text_copy;
					node->next = NULL;
					if (queue_tail) queue_tail->next = node;
					else            queue_head = node;
					queue_tail = node;
					queue_count++;
					QUEUE_REFRESH_INDICATOR();
				} else {
					free(node);
					free(text_copy);
				}
				free(input);
			} else if (!current_model) {
				tui_info("No model selected. Press 'm' in NORMAL mode first.");
				free(input);
			} else {
				if (session->db_id < 0) {
					char cwd[512];
					if (!getcwd(cwd, sizeof(cwd))) cwd[0] = '\0';
					session->db_id = db_session_create("chat",
						current_model ? current_model : "", cwd);
					refresh_sessions_pane(session->db_id);
					refresh_session_header(session);
				}

				kora_session_add(session, "user", input);
				session->user_msg_count++;

				if (session->db_id >= 0)
					db_message_add(session->db_id, session->msg_seq++,
					               "user", input, current_model, 1);

				int tok = kora_session_approx_tokens(session);
				update_context_status(session, cfg->ctx_size);

				if (ctx_needs_compression(tok, cfg->ctx_size) && !compacting) {
					tui_log("Context approaching limit; compacting in background…");
					launch_compact(base_url, current_model, session,
					               cfg->compact_chat, tok);
				}

				if (launch_gen(base_url, current_model, session) != 0)
					tui_log("Out of memory.");

				free(input);
			}
		}

		/* signal + join the daemon poller */
		g_poller_stop = 1;
		pthread_join(poller_tid, NULL);

		gen_cancel();
		compact_cancel();
		while (naming_session) {
			struct timespec ts = {0, 50 * 1000 * 1000};
			nanosleep(&ts, NULL);
		}

		free(gen_response);     gen_response = NULL;
		free(compact_summary);  compact_summary = NULL;
		free(session_name_result); session_name_result = NULL;
		while (queue_head) {
			struct pending_msg *node = queue_head;
			queue_head = node->next;
			free(node->text);
			free(node);
		}

		tui_cleanup();
		kora_session_free(session);
		free(current_model);
		kora_config_free(cfg);
		db_close();
		return 0;
	}

	fprintf(stderr, "kora: unknown command '%s'\n", argv[1]);
	usage();
	return 1;
}
