#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "chat_native.h"
#include "guards.h"
#include "inference.h"
#include "loop.h"
#include "lua_bridge.h"
#include "parser.h"
#include "run.h"
#include "tui.h"
#include "util.h"

/* ===== debug log =====
   while we don't yet persist code-mode sessions to sqlite, write a raw
   trace of every turn (prompt, output, tool call, result) to ~/.kora/code.log
   so we can post-mortem buggy runs. */

static FILE *log_fp = NULL;

static void log_open(void)
{
	if (log_fp)
		return;
	char *p = kora_path("code.log");
	if (!p)
		return;
	log_fp = fopen(p, "a");
	free(p);
	if (log_fp) {
		time_t t = time(NULL);
		fprintf(log_fp, "\n========== %s", ctime(&t));
		fflush(log_fp);
	}
}

static void log_section(const char *label, const char *body)
{
	if (!log_fp || !body) return;
	fprintf(log_fp, "----- %s -----\n%s\n", label, body);
	fflush(log_fp);
}

/* ===== streaming callback bridge =====
   kora_generate uses a global callback. we set it for the duration of one
   inference turn, route tokens into the parser, and abort generation as
   soon as a complete <tool_call> arrives. */

struct stream_ctx {
	struct kora_parser *parser;
	struct kora_run *run;
};

static void agent_stream_cb(const char *text, int len, void *user_data)
{
	struct stream_ctx *sc = user_data;
	if (sc->run->show_in_tui)
		tui_assistant_chunk(text, len);
	/* parser is NULL in native mode (no incremental marker scanning) */
	if (sc->parser && kora_parser_feed(sc->parser, text, len)) {
		/* tool call complete — stop generation */
		kora_abort();
	}
}

/* ===== prompt builder =====
   compose the message list into a templated prompt for the model. */

static char *build_prompt(struct kora_run *r)
{
	const char **roles = malloc(sizeof(char *) * r->n_msgs);
	const char **contents = malloc(sizeof(char *) * r->n_msgs);
	if (!roles || !contents) {
		free(roles); free(contents);
		return NULL;
	}
	int i;
	for (i = 0; i < r->n_msgs; i++) {
		roles[i] = r->msgs[i].role;
		contents[i] = r->msgs[i].content;
	}
	char *out = kora_apply_template(r->kc, roles, contents, r->n_msgs);
	free(roles);
	free(contents);
	return out;
}

/* extract the tool name from a JSON object string. very forgiving:
   looks for "name"<ws>:<ws>"value". returns allocated string or NULL. */
static char *extract_tool_name(const char *json)
{
	const char *p = strstr(json, "\"name\"");
	if (!p) return NULL;
	p += 6;
	while (*p && *p != ':') p++;
	if (!*p) return NULL;
	p++;
	while (*p == ' ' || *p == '\t' || *p == '\n') p++;
	if (*p != '"') return NULL;
	p++;
	const char *end = p;
	while (*end && *end != '"') {
		if (*end == '\\' && end[1]) end++;
		end++;
	}
	size_t len = end - p;
	char *out = malloc(len + 1);
	memcpy(out, p, len);
	out[len] = '\0';
	return out;
}

/* extract the "arguments" object as a substring (with braces). */
static char *extract_tool_args(const char *json)
{
	const char *p = strstr(json, "\"arguments\"");
	if (!p) return strdup("{}");
	p += 11;
	while (*p && *p != ':') p++;
	if (!*p) return strdup("{}");
	p++;
	while (*p == ' ' || *p == '\t' || *p == '\n') p++;
	if (*p != '{') return strdup("{}");
	int depth = 0;
	const char *start = p;
	int in_str = 0;
	while (*p) {
		if (in_str) {
			if (*p == '\\' && p[1]) { p += 2; continue; }
			if (*p == '"') in_str = 0;
		} else {
			if (*p == '"') in_str = 1;
			else if (*p == '{') depth++;
			else if (*p == '}') {
				depth--;
				if (depth == 0) { p++; break; }
			}
		}
		p++;
	}
	size_t len = p - start;
	char *out = malloc(len + 1);
	memcpy(out, start, len);
	out[len] = '\0';
	return out;
}

/* ===== main loop ===== */

char *kora_loop_run(struct kora_run *r, const char *user_prompt, char **err_out)
{
	if (err_out) *err_out = NULL;
	log_open();

	if (r->depth >= KORA_MAX_DEPTH) {
		if (err_out) *err_out = strdup("max sub-agent depth exceeded");
		return NULL;
	}

	/* The system message is pushed exactly once per run lifetime.
	   On subsequent calls (continuation of the same conversation), we
	   skip the system push and just append the new user message — same
	   pattern as a chat conversation: system at index 0, then alternating
	   user/assistant messages thereafter. */
	int is_first_call = (r->n_msgs == 0);
	if (is_first_call) {
		const char *mode_str = (r->mode == KORA_MODE_NATIVE) ? "native" : "harness";
		char *system_prompt = kora_lua_build_system_prompt(r->agent_name, mode_str);
		if (!system_prompt) {
			if (err_out) *err_out = strdup("failed to build system prompt");
			return NULL;
		}
		kora_run_push_msg(r, "system", system_prompt, NULL);
		if (log_fp) {
			fprintf(log_fp, "===== run start: agent=%s depth=%d =====\n",
				r->agent_name, r->depth);
			log_section("SYSTEM", system_prompt);
		}
		free(system_prompt);
	} else if (log_fp) {
		fprintf(log_fp, "===== run continue: agent=%s msgs=%d =====\n",
			r->agent_name, r->n_msgs);
	}
	if (log_fp)
		log_section("USER", user_prompt);

	if (user_prompt && *user_prompt)
		kora_run_push_msg(r, "user", user_prompt, NULL);

	/* set up the per-run tool whitelist gate (read by lua dispatcher) */
	kora_lua_set_run_tool_whitelist(r->agent_name);
	kora_lua_set_current_run(r);

	/* reset per-call guard counters but keep the always-allow list so the
	   user isn't re-asked for previously approved tools across messages. */
	kora_guards_reset_per_call(r);

	struct kora_parser parser;
	kora_parser_init(&parser);

	struct stream_ctx sc = { .parser = &parser, .run = r };

	char *final_text = NULL;
	int aborted = 0;

	while (r->steps < KORA_MAX_STEPS && !aborted) {
		if (r->cancel && *r->cancel)
			break;

		/* ===== NATIVE PATH =====
		   uses llama.cpp's common_chat_templates_apply with structured
		   tools, generates fully, then parses the result with
		   common_chat_parse to extract structured tool calls. no manual
		   <tool_call> marker scanning. */
		if (r->mode == KORA_MODE_NATIVE && r->kc->native) {
			/* collect tools from the agent's whitelist (in lua) */
			char **tnames = NULL, **tdescs = NULL, **tschemas = NULL;
			int n_tools = kora_lua_collect_tools(r->agent_name,
				&tnames, &tdescs, &tschemas);
			if (n_tools < 0) n_tools = 0;

			/* build messages array from r->msgs */
			const char **roles = malloc(sizeof(char *) * r->n_msgs);
			const char **contents = malloc(sizeof(char *) * r->n_msgs);
			if (!roles || !contents) {
				free(roles); free(contents);
				kora_lua_tools_free(tnames, tdescs, tschemas, n_tools);
				if (err_out) *err_out = strdup("oom");
				break;
			}
			for (int i = 0; i < r->n_msgs; i++) {
				roles[i] = r->msgs[i].role;
				contents[i] = r->msgs[i].content;
			}

			struct kora_native_apply_result apply = kora_native_chat_apply(
				r->kc->native,
				roles, contents, r->n_msgs,
				(const char **)tnames, (const char **)tdescs,
				(const char **)tschemas, n_tools);
			free(roles); free(contents);

			if (!apply.prompt) {
				kora_lua_tools_free(tnames, tdescs, tschemas, n_tools);
				if (err_out) *err_out = strdup("native chat apply failed");
				break;
			}

			if (log_fp) {
				fprintf(log_fp, "----- TURN %d NATIVE PROMPT (fmt=%d) -----\n%s\n",
					r->steps, apply.format, apply.prompt);
				fflush(log_fp);
			}

			/* stream raw output to TUI; no parser feed */
			sc.parser = NULL;
			kora_set_stream_cb(agent_stream_cb, &sc);
			kora_clear(r->kc);
			kora_abort_reset();
			if (r->show_in_tui)
				tui_assistant_begin();

			char *full = NULL;
			kora_generate(r->kc, apply.prompt, &full);
			kora_set_stream_cb(NULL, NULL);
			if (r->show_in_tui)
				tui_assistant_end();

			int format = apply.format;
			kora_native_apply_result_free(&apply);
			r->steps++;

			if (log_fp) {
				fprintf(log_fp, "----- TURN %d NATIVE OUTPUT -----\n%s\n",
					r->steps - 1, full ? full : "(null)");
				fflush(log_fp);
			}

			/* parse the model's full response into content + tool_calls */
			struct kora_native_parse_result parsed = kora_native_chat_parse(
				r->kc->native, full ? full : "", format);

			kora_lua_tools_free(tnames, tdescs, tschemas, n_tools);

			if (log_fp) {
				fprintf(log_fp, "----- NATIVE PARSED: tool_calls=%d, content_len=%d -----\n",
					parsed.n_tool_calls,
					parsed.content ? (int)strlen(parsed.content) : 0);
				fflush(log_fp);
			}

			if (parsed.n_tool_calls == 0) {
				/* no tool calls — final response */
				const char *out = (parsed.content && *parsed.content)
					? parsed.content
					: (full ? full : "");
				if (*out) {
					kora_run_push_msg(r, "assistant", out, NULL);
					final_text = strdup(out);
				} else if (r->show_in_tui) {
					/* model produced empty output — usually means the
					   prompt was too large for the context. surface a
					   clear hint instead of silently doing nothing. */
					tui_info("(empty response — try /clear to reset the conversation, "
					         "the prompt may have exceeded the model context)");
				}
				free(full);
				kora_native_parse_result_free(&parsed);
				break;
			}

			/* tool calls — push raw assistant text (preserves the model's
			   own format markup so the next turn renders consistently),
			   then dispatch each call and append results */
			kora_run_push_msg(r, "assistant", full ? full : "", NULL);
			free(full);

			int native_break = 0;
			for (int ti = 0; ti < parsed.n_tool_calls && !native_break; ti++) {
				const char *tname = parsed.tool_calls[ti].name;
				const char *targs = parsed.tool_calls[ti].arguments_json;
				if (!tname) tname = "";
				if (!targs) targs = "{}";

				if (r->show_in_tui)
					tui_tool_call(tname, targs);

				struct kora_call call = { .name = tname, .args_json = targs, .run = r };
				struct kora_guard_result v = kora_guards_check(&call);

				char *result_json = NULL;
				if (v.verdict == KORA_VERDICT_ABORT) {
					if (r->show_in_tui) {
						char buf[256];
						snprintf(buf, sizeof(buf), "Aborting: %s",
							v.reason ? v.reason : "guard rejected");
						tui_info(buf);
					}
					if (err_out && !*err_out)
						*err_out = v.reason ? strdup(v.reason) : strdup("guard abort");
					free(v.reason);
					aborted = 1;
					native_break = 1;
					break;
				} else if (v.verdict == KORA_VERDICT_DENY) {
					asprintf(&result_json,
						"{\"ok\":false,\"error\":\"%s\"}",
						v.reason ? v.reason : "denied by guard");
					free(v.reason);
				} else {
					free(v.reason);
					result_json = kora_lua_dispatch_tool(tname, targs);
					kora_guards_record_result(r, result_json);
				}

				if (log_fp) {
					fprintf(log_fp, "----- TOOL CALL (native) -----\nname=%s args=%s\nresult=%s\n",
						tname, targs, result_json ? result_json : "(null)");
					fflush(log_fp);
				}
				if (r->show_in_tui && result_json)
					tui_tool_result(result_json, 1);

				/* push the tool result as a "tool" role message — common_chat
				   formats this with the right wrapper for the model's template */
				kora_run_push_msg(r, "tool",
					result_json ? result_json : "{\"ok\":false}",
					tname);
				free(result_json);
			}

			kora_native_parse_result_free(&parsed);
			if (native_break) break;
			continue;  /* next iteration: rebuild prompt with appended messages */
		}

		/* ===== HARNESS PATH (fallback) ===== */
		char *prompt = build_prompt(r);
		if (!prompt) {
			if (err_out) *err_out = strdup("failed to apply chat template");
			break;
		}

		if (log_fp) {
			fprintf(log_fp, "----- TURN %d PROMPT -----\n%s\n",
				r->steps, prompt);
			fflush(log_fp);
		}

		kora_parser_reset(&parser);
		sc.parser = &parser;
		kora_set_stream_cb(agent_stream_cb, &sc);
		kora_clear(r->kc);
		kora_abort_reset();

		if (r->show_in_tui)
			tui_assistant_begin();

		char *full = NULL;
		kora_generate(r->kc, prompt, &full);
		free(prompt);
		kora_set_stream_cb(NULL, NULL);

		/* If the model emitted <tool_call>+body then ended its turn at the
		   token level (Llama-3 <|eot_id|>) without writing the close marker,
		   finalize the parser so the buffered body is treated as the call. */
		kora_parser_finalize(&parser);

		if (r->show_in_tui)
			tui_assistant_end();

		r->steps++;

		if (log_fp) {
			fprintf(log_fp, "----- TURN %d OUTPUT (state=%d) -----\n%s\n",
				r->steps - 1, parser.state, full ? full : "(null)");
			fflush(log_fp);
		}

		if (parser.state == KORA_PARSE_DONE) {
			/* tool call: append the assistant text (pre-call) and the tool call,
			   then dispatch and append the result */
			const char *text_part = kora_parser_text(&parser);
			char *assistant_buf = NULL;
			asprintf(&assistant_buf, "%s%s%s%s%s",
				text_part ? text_part : "",
				"\n<tool_call>",
				parser.call_body ? parser.call_body : "",
				"</tool_call>",
				"");
			kora_run_push_msg(r, "assistant", assistant_buf, NULL);
			free(assistant_buf);

			char *name = extract_tool_name(parser.call_body);
			char *args = extract_tool_args(parser.call_body);

			/* salvage path: hand the raw body to Lua, which knows the
			   alternative formats (XML-style nested tags, etc) */
			const char *dispatch_name = name ? name : "";
			const char *dispatch_args = name ? args : parser.call_body;

			if (r->show_in_tui)
				tui_tool_call(dispatch_name, dispatch_args);

			/* run the guard pipeline */
			struct kora_call call = {
				.name = dispatch_name,
				.args_json = dispatch_args,
				.run = r,
			};
			struct kora_guard_result v = kora_guards_check(&call);

			char *result_json = NULL;
			if (v.verdict == KORA_VERDICT_ABORT) {
				if (r->show_in_tui) {
					char buf[256];
					snprintf(buf, sizeof(buf), "Aborting: %s",
						v.reason ? v.reason : "guard rejected");
					tui_info(buf);
				}
				if (err_out && !*err_out)
					*err_out = v.reason ? strdup(v.reason) : strdup("guard abort");
				free(v.reason);
				aborted = 1;
				free(name); free(args); free(full);
				break;
			} else if (v.verdict == KORA_VERDICT_DENY) {
				asprintf(&result_json,
					"{\"ok\":false,\"error\":\"%s\"}",
					v.reason ? v.reason : "denied by guard");
				free(v.reason);
			} else {
				/* ALLOW (ASK was already converted to ALLOW or DENY by the
				   permission guard before returning) */
				free(v.reason);
				result_json = kora_lua_dispatch_tool(dispatch_name, dispatch_args);
				kora_guards_record_result(r, result_json);
			}

			if (log_fp) {
				fprintf(log_fp, "----- TOOL CALL -----\nname=%s args=%s\nresult=%s\n",
					dispatch_name, dispatch_args,
					result_json ? result_json : "(null)");
				fflush(log_fp);
			}
			if (r->show_in_tui && result_json)
				tui_tool_result(result_json, 1);

			char *wrapped = NULL;
			asprintf(&wrapped, "<tool_result>%s</tool_result>",
				result_json ? result_json : "{\"ok\":false}");
			kora_run_push_msg(r, "user", wrapped, NULL);
			free(wrapped);
			free(result_json);
			free(name);
			free(args);
			free(full);
			continue;
		}

		/* no tool call — done */
		if (full && *full) {
			kora_run_push_msg(r, "assistant", full, NULL);
			final_text = full;
		} else {
			free(full);
		}
		break;
	}

	kora_parser_free(&parser);
	kora_lua_clear_run_tool_whitelist();
	kora_lua_set_current_run(r->parent);
	/* per-run guard state (always-allow list) is preserved for the next
	   call. it gets freed when the run itself is freed via kora_run_free. */

	/* reset the steps counter so the next user message starts fresh */
	r->steps = 0;

	if (!final_text && r->steps >= KORA_MAX_STEPS && err_out && !*err_out) {
		*err_out = strdup("step limit exceeded");
	}
	return final_text;
}

char *kora_loop_run_subagent(struct kora_run *parent,
                             const char *agent_name,
                             const char *prompt,
                             char **err_out)
{
	if (!kora_lua_agent_exists(agent_name, parent->agent_name)) {
		if (err_out) {
			asprintf(err_out, "agent '%s' not declared by '%s'",
				agent_name, parent->agent_name ? parent->agent_name : "(top)");
		}
		return NULL;
	}

	struct kora_run *child = kora_run_new(parent->kc, parent->cancel, parent);
	if (!child) {
		if (err_out) *err_out = strdup("failed to allocate run");
		return NULL;
	}
	child->agent_name = agent_name;

	char *result = kora_loop_run(child, prompt, err_out);
	kora_run_free(child);
	return result;
}
