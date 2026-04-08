#ifndef KORA_CHAT_NATIVE_H
#define KORA_CHAT_NATIVE_H

/* Thin C wrapper around llama.cpp's common_chat_templates_* API.
   Provides native tool calling: the model's own training format is used
   (Qwen / Llama 3.x / Hermes / Mistral / etc), parsed by llama.cpp.
   This replaces the harness `<tool_call>` text-marker approach with
   model-native serialization, dramatically reducing format errors. */

#ifdef __cplusplus
extern "C" {
#endif

struct llama_model;
struct kora_native_chat;

/* one parsed tool call from the model's output */
struct kora_native_tool_call {
	char *name;
	char *arguments_json;  /* JSON object string */
};

/* result of common_chat_templates_apply: a fully formatted prompt
   ready to tokenize, plus the format ID we'll need later for parsing. */
struct kora_native_apply_result {
	char *prompt;          /* allocated; caller frees */
	int format;            /* opaque common_chat_format integer */
	int has_grammar;       /* 1 if a grammar was returned */
	char *grammar;         /* allocated; may be NULL */
};

/* result of common_chat_parse on the model's response */
struct kora_native_parse_result {
	char *content;                              /* plain text content */
	struct kora_native_tool_call *tool_calls;   /* allocated array */
	int n_tool_calls;
};

/* lifetime: create after llama_model is loaded, free before model is freed */
struct kora_native_chat *kora_native_chat_create(struct llama_model *model);
void kora_native_chat_free(struct kora_native_chat *nc);

/* probe: returns 1 if the model's template can do tool calling natively
   (i.e. its detected format is anything other than CONTENT_ONLY) */
int kora_native_chat_supports_tools(struct kora_native_chat *nc);

/* returns the human-readable name of the detected format
   (e.g. "Hermes 2 Pro", "Llama 3.x", "Content-only") */
const char *kora_native_chat_format_name(struct kora_native_chat *nc);

/* build the prompt + format params for one inference call.
   roles[i]/contents[i] is the message array. tool arrays may be NULL/0
   for a no-tools call. tool_schemas[i] is a JSON Schema string for
   the tool's parameters. caller must free the returned struct's fields. */
struct kora_native_apply_result kora_native_chat_apply(
	struct kora_native_chat *nc,
	const char **roles, const char **contents, int n_msgs,
	const char **tool_names, const char **tool_descs, const char **tool_schemas,
	int n_tools);

void kora_native_apply_result_free(struct kora_native_apply_result *r);

/* parse the model's full response text into content + tool calls.
   format must be the value returned by kora_native_chat_apply. */
struct kora_native_parse_result kora_native_chat_parse(
	struct kora_native_chat *nc,
	const char *text,
	int format);

void kora_native_parse_result_free(struct kora_native_parse_result *r);

#ifdef __cplusplus
}
#endif

#endif
