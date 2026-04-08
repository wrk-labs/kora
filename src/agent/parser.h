#ifndef KORA_PARSER_H
#define KORA_PARSER_H

#include <stddef.h>

/* streaming tool-call parser.
   fed token chunks as they arrive from the model, watches for the
   <tool_call>...</tool_call> markers and extracts the body. */

#define KORA_TC_OPEN  "<tool_call>"
#define KORA_TC_CLOSE "</tool_call>"

enum kora_parser_state {
	KORA_PARSE_TEXT = 0,    /* normal assistant text */
	KORA_PARSE_INSIDE,      /* inside a tool_call block */
	KORA_PARSE_DONE,        /* a complete tool_call body has been captured */
};

struct kora_parser {
	enum kora_parser_state state;

	/* accumulator for the entire stream so far */
	char *buf;
	size_t buf_len;
	size_t buf_cap;

	/* extracted tool-call body (between the markers, NUL-terminated) */
	char *call_body;
	size_t call_body_len;

	/* offset in buf where the assistant text ends (start of <tool_call>) */
	size_t text_end;
};

void kora_parser_init(struct kora_parser *p);
void kora_parser_free(struct kora_parser *p);

/* feed a chunk of streamed text. returns 1 if a tool call is now complete
   (state == DONE), 0 otherwise. when 1 is returned the caller should abort
   generation, consume call_body, then reset the parser before next turn. */
int kora_parser_feed(struct kora_parser *p, const char *text, int len);

/* return the assistant text portion (everything before <tool_call>),
   NUL-terminated, owned by the parser. */
const char *kora_parser_text(struct kora_parser *p);

/* called after generation ends. if the parser is still INSIDE (model
   emitted <tool_call> + body but ended its turn before writing the close
   marker, which happens with Llama-3's <|eot_id|> end-of-turn), close the
   block by treating the buffered body as the complete call.
   returns 1 if a tool call is now available, 0 otherwise. */
int kora_parser_finalize(struct kora_parser *p);

/* reset for the next turn. keeps allocations. */
void kora_parser_reset(struct kora_parser *p);

#endif
