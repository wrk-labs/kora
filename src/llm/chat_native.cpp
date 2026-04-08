// Thin C++ wrapper around llama.cpp's common_chat_* API.
//
// This file is built with c++ and exposes a C interface so the rest of
// kora (which is C) can call native tool calling without dragging in
// the C++ chat headers everywhere.

#include "chat_native.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/chat.h"

extern "C" {

struct kora_native_chat {
	common_chat_templates_ptr tmpls;
	struct llama_model *model;
};

static char *dup_cstr(const std::string &s)
{
	char *out = (char *)std::malloc(s.size() + 1);
	if (!out) return nullptr;
	std::memcpy(out, s.c_str(), s.size());
	out[s.size()] = '\0';
	return out;
}

struct kora_native_chat *kora_native_chat_create(struct llama_model *model)
{
	if (!model) return nullptr;
	auto tmpls = common_chat_templates_init(model, "");
	if (!tmpls) return nullptr;
	auto *nc = new kora_native_chat;
	nc->tmpls = std::move(tmpls);
	nc->model = model;
	return nc;
}

void kora_native_chat_free(struct kora_native_chat *nc)
{
	if (!nc) return;
	delete nc;  // tmpls is unique_ptr, releases automatically
}

/* Probe by doing a minimal apply with a single user message and an
   empty tools list, then look at the format ID. CONTENT_ONLY means the
   template doesn't know about tools — anything else means it does. */
int kora_native_chat_supports_tools(struct kora_native_chat *nc)
{
	if (!nc || !nc->tmpls) return 0;

	common_chat_templates_inputs inputs;
	inputs.use_jinja = true;
	common_chat_msg probe;
	probe.role = "user";
	probe.content = "ping";
	inputs.messages.push_back(probe);

	// add one fake tool so the template can pick its tool format
	common_chat_tool t;
	t.name = "noop";
	t.description = "no-op";
	t.parameters = "{\"type\":\"object\",\"properties\":{}}";
	inputs.tools.push_back(t);
	inputs.tool_choice = COMMON_CHAT_TOOL_CHOICE_AUTO;

	try {
		auto params = common_chat_templates_apply(nc->tmpls.get(), inputs);
		return params.format != COMMON_CHAT_FORMAT_CONTENT_ONLY ? 1 : 0;
	} catch (const std::exception &) {
		return 0;
	}
}

const char *kora_native_chat_format_name(struct kora_native_chat *nc)
{
	if (!nc || !nc->tmpls) return "unknown";

	common_chat_templates_inputs inputs;
	inputs.use_jinja = true;
	common_chat_msg probe;
	probe.role = "user";
	probe.content = "ping";
	inputs.messages.push_back(probe);
	common_chat_tool t;
	t.name = "noop";
	t.description = "no-op";
	t.parameters = "{\"type\":\"object\",\"properties\":{}}";
	inputs.tools.push_back(t);
	inputs.tool_choice = COMMON_CHAT_TOOL_CHOICE_AUTO;

	try {
		auto params = common_chat_templates_apply(nc->tmpls.get(), inputs);
		return common_chat_format_name(params.format);
	} catch (const std::exception &) {
		return "error";
	}
}

struct kora_native_apply_result kora_native_chat_apply(
	struct kora_native_chat *nc,
	const char **roles, const char **contents, int n_msgs,
	const char **tool_names, const char **tool_descs, const char **tool_schemas,
	int n_tools)
{
	kora_native_apply_result out{};
	if (!nc || !nc->tmpls) return out;

	common_chat_templates_inputs inputs;
	inputs.use_jinja = true;
	inputs.add_generation_prompt = true;

	for (int i = 0; i < n_msgs; i++) {
		common_chat_msg m;
		m.role = roles[i] ? roles[i] : "user";
		m.content = contents[i] ? contents[i] : "";
		inputs.messages.push_back(m);
	}

	for (int i = 0; i < n_tools; i++) {
		common_chat_tool t;
		t.name = tool_names[i] ? tool_names[i] : "";
		t.description = tool_descs[i] ? tool_descs[i] : "";
		t.parameters = tool_schemas[i] ? tool_schemas[i]
		                               : "{\"type\":\"object\",\"properties\":{}}";
		inputs.tools.push_back(t);
	}
	if (n_tools > 0)
		inputs.tool_choice = COMMON_CHAT_TOOL_CHOICE_AUTO;

	try {
		auto params = common_chat_templates_apply(nc->tmpls.get(), inputs);
		out.prompt = dup_cstr(params.prompt);
		out.format = (int)params.format;
		out.has_grammar = params.grammar.empty() ? 0 : 1;
		out.grammar = params.grammar.empty() ? nullptr : dup_cstr(params.grammar);
	} catch (const std::exception &e) {
		std::fprintf(stderr, "kora_native_chat_apply: %s\n", e.what());
	}
	return out;
}

void kora_native_apply_result_free(struct kora_native_apply_result *r)
{
	if (!r) return;
	std::free(r->prompt);
	std::free(r->grammar);
	r->prompt = nullptr;
	r->grammar = nullptr;
}

struct kora_native_parse_result kora_native_chat_parse(
	struct kora_native_chat *nc,
	const char *text,
	int format)
{
	kora_native_parse_result out{};
	if (!nc || !text) return out;

	common_chat_parser_params syntax;
	syntax.format = (common_chat_format)format;
	syntax.parse_tool_calls = true;

	try {
		auto msg = common_chat_parse(text, /*is_partial=*/false, syntax);
		out.content = dup_cstr(msg.content);
		out.n_tool_calls = (int)msg.tool_calls.size();
		if (out.n_tool_calls > 0) {
			out.tool_calls = (kora_native_tool_call *)std::calloc(
				out.n_tool_calls, sizeof(kora_native_tool_call));
			for (int i = 0; i < out.n_tool_calls; i++) {
				out.tool_calls[i].name = dup_cstr(msg.tool_calls[i].name);
				out.tool_calls[i].arguments_json = dup_cstr(msg.tool_calls[i].arguments);
			}
		}
	} catch (const std::exception &e) {
		std::fprintf(stderr, "kora_native_chat_parse: %s\n", e.what());
		out.content = dup_cstr(std::string(text));
	}
	return out;
}

void kora_native_parse_result_free(struct kora_native_parse_result *r)
{
	if (!r) return;
	std::free(r->content);
	for (int i = 0; i < r->n_tool_calls; i++) {
		std::free(r->tool_calls[i].name);
		std::free(r->tool_calls[i].arguments_json);
	}
	std::free(r->tool_calls);
	r->content = nullptr;
	r->tool_calls = nullptr;
	r->n_tool_calls = 0;
}

} // extern "C"
