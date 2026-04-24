#include <stdlib.h>
#include <string.h>

#include "md4c.h"
#include "markdown.h"

#define MAX_INLINE_DEPTH 16
#define MAX_LIST_DEPTH   8

struct ctx {
	kora_md_emit_fn cb;
	void           *user_data;

	/* inline attribute stack — spans nest, so we snapshot and restore. */
	int attr_current;
	int attr_stack[MAX_INLINE_DEPTH];
	int attr_depth;

	/* list nesting depth — reported to the consumer on LIST_ITEM_BEGIN so
	   it can indent appropriately without tracking state itself. */
	int list_depth;

	/* are we inside MD_BLOCK_CODE? md4c re-uses MD_TEXT_CODE for both
	   inline code spans and code blocks, so we need the ambient block
	   context to tell them apart. md4c emits code-block content as a
	   stream of small fragments with '\n' as its own event, so we buffer
	   up to a newline and emit one CODE_BLOCK_LINE per logical line. */
	int    in_code_block;
	char  *code_line;
	size_t code_line_len;
	size_t code_line_cap;

	/* emit a PARAGRAPH_BREAK lazily before the next block, so the caller
	   never sees a leading blank nor a trailing blank. */
	int pending_break;

	/* suppress paragraph breaks inside tables — table contents shouldn't
	   have blank-line rhythm between rows / cells. */
	int in_table;
};

static void emit(struct ctx *c, int kind, int attrs, int level,
                 const char *text, size_t len);

static void code_line_append(struct ctx *c, const char *text, size_t n)
{
	if (n == 0) return;
	if (c->code_line_len + n + 1 > c->code_line_cap) {
		size_t nc = c->code_line_cap ? c->code_line_cap * 2 : 128;
		while (nc < c->code_line_len + n + 1) nc *= 2;
		char *nb = realloc(c->code_line, nc);
		if (!nb) return;  /* truncate the line silently on OOM */
		c->code_line = nb;
		c->code_line_cap = nc;
	}
	memcpy(c->code_line + c->code_line_len, text, n);
	c->code_line_len += n;
}

static void code_line_flush(struct ctx *c)
{
	emit(c, KMD_CODE_BLOCK_LINE, 0, 0,
	     c->code_line ? c->code_line : "",
	     c->code_line_len);
	c->code_line_len = 0;
}

static void emit(struct ctx *c, int kind, int attrs, int level,
                 const char *text, size_t len)
{
	c->cb(kind, attrs, level, text, len, c->user_data);
}



static void maybe_break(struct ctx *c)
{
	if (c->in_table) {
		c->pending_break = 0;
		return;
	}
	if (c->pending_break) {
		emit(c, KMD_PARAGRAPH_BREAK, 0, 0, NULL, 0);
		c->pending_break = 0;
	}
}

static int on_enter_block(MD_BLOCKTYPE type, void *detail, void *user)
{
	struct ctx *c = user;
	switch (type) {
	case MD_BLOCK_DOC:
		break;
	case MD_BLOCK_H: {
		MD_BLOCK_H_DETAIL *d = detail;
		maybe_break(c);
		emit(c, KMD_HEADING_BEGIN, 0, (int)d->level, NULL, 0);
		break;
	}
	case MD_BLOCK_P:
		maybe_break(c);
		break;
	case MD_BLOCK_CODE: {
		MD_BLOCK_CODE_DETAIL *d = detail;
		maybe_break(c);
		c->in_code_block = 1;
		emit(c, KMD_CODE_BLOCK_BEGIN, 0, 0,
		     d->lang.text ? d->lang.text : "",
		     d->lang.text ? (size_t)d->lang.size : 0);
		break;
	}
	case MD_BLOCK_UL:
	case MD_BLOCK_OL:
		maybe_break(c);
		if (c->list_depth < MAX_LIST_DEPTH) c->list_depth++;
		break;
	case MD_BLOCK_LI:
		emit(c, KMD_LIST_ITEM_BEGIN, 0, c->list_depth, NULL, 0);
		break;
	case MD_BLOCK_QUOTE:
		maybe_break(c);
		emit(c, KMD_BLOCKQUOTE_BEGIN, 0, 0, NULL, 0);
		break;
	case MD_BLOCK_TABLE:
		maybe_break(c);
		c->in_table = 1;
		emit(c, KMD_TABLE_BEGIN, 0, 0, NULL, 0);
		break;
	case MD_BLOCK_THEAD:
	case MD_BLOCK_TBODY:
		/* structural groupings — children emit their own events. */
		break;
	case MD_BLOCK_TR:
		emit(c, KMD_TABLE_ROW_BEGIN, 0, 0, NULL, 0);
		break;
	case MD_BLOCK_TH:
		emit(c, KMD_TABLE_CELL_BEGIN, 0, 1, NULL, 0);
		break;
	case MD_BLOCK_TD:
		emit(c, KMD_TABLE_CELL_BEGIN, 0, 0, NULL, 0);
		break;
	default:
		break;  /* HR / HTML — silently dropped */
	}
	return 0;
}

static int on_leave_block(MD_BLOCKTYPE type, void *detail, void *user)
{
	(void)detail;
	struct ctx *c = user;
	switch (type) {
	case MD_BLOCK_H:
		emit(c, KMD_HEADING_END, 0, 0, NULL, 0);
		c->pending_break = 1;
		break;
	case MD_BLOCK_P:
		c->pending_break = 1;
		break;
	case MD_BLOCK_CODE:
		/* flush any trailing line without a terminating newline (rare,
		   but md4c does emit it when the fence closes mid-line). */
		if (c->code_line_len > 0)
			code_line_flush(c);
		emit(c, KMD_CODE_BLOCK_END, 0, 0, NULL, 0);
		c->in_code_block = 0;
		c->pending_break = 1;
		break;
	case MD_BLOCK_LI:
		emit(c, KMD_LIST_ITEM_END, 0, 0, NULL, 0);
		break;
	case MD_BLOCK_UL:
	case MD_BLOCK_OL:
		if (c->list_depth > 0) c->list_depth--;
		c->pending_break = 1;
		break;
	case MD_BLOCK_QUOTE:
		emit(c, KMD_BLOCKQUOTE_END, 0, 0, NULL, 0);
		c->pending_break = 1;
		break;
	case MD_BLOCK_TABLE:
		emit(c, KMD_TABLE_END, 0, 0, NULL, 0);
		c->in_table = 0;
		c->pending_break = 1;
		break;
	case MD_BLOCK_TR:
		emit(c, KMD_TABLE_ROW_END, 0, 0, NULL, 0);
		break;
	case MD_BLOCK_TH:
	case MD_BLOCK_TD:
		emit(c, KMD_TABLE_CELL_END, 0, 0, NULL, 0);
		break;
	default:
		break;
	}
	return 0;
}

static int on_enter_span(MD_SPANTYPE type, void *detail, void *user)
{
	(void)detail;
	struct ctx *c = user;
	int bit = 0;
	switch (type) {
	case MD_SPAN_STRONG: bit = KMD_ATTR_BOLD;        break;
	case MD_SPAN_EM:     bit = KMD_ATTR_ITALIC;      break;
	case MD_SPAN_CODE:   bit = KMD_ATTR_CODE_INLINE; break;
	default: return 0;  /* A / IMG / DEL / latex / wiki — passthrough */
	}
	if (c->attr_depth < MAX_INLINE_DEPTH) {
		c->attr_stack[c->attr_depth++] = c->attr_current;
		c->attr_current |= bit;
	}
	return 0;
}

static int on_leave_span(MD_SPANTYPE type, void *detail, void *user)
{
	(void)detail;
	struct ctx *c = user;
	switch (type) {
	case MD_SPAN_STRONG:
	case MD_SPAN_EM:
	case MD_SPAN_CODE:
		if (c->attr_depth > 0)
			c->attr_current = c->attr_stack[--c->attr_depth];
		break;
	default:
		break;
	}
	return 0;
}

static int on_text(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *user)
{
	struct ctx *c = user;

	if (c->in_code_block) {
		/* md4c emits code-block content as many small fragments with
		   '\n' as its own event. buffer until we see the newline, then
		   flush as one KMD_CODE_BLOCK_LINE. */
		if (size == 1 && text[0] == '\n') {
			code_line_flush(c);
		} else {
			code_line_append(c, text, size);
		}
		return 0;
	}

	switch (type) {
	case MD_TEXT_BR:
		emit(c, KMD_HARD_BREAK, 0, 0, NULL, 0);
		break;
	case MD_TEXT_SOFTBR:
		/* soft line break inside a paragraph — collapse to a single
		   space so wrap logic in the pad owns line breaking. */
		emit(c, KMD_TEXT, c->attr_current, 0, " ", 1);
		break;
	case MD_TEXT_CODE:
		/* MD_TEXT_CODE outside MD_BLOCK_CODE means inline `code` — the
		   CODE_INLINE attr was already pushed by enter_span. */
		emit(c, KMD_TEXT, c->attr_current, 0, text, size);
		break;
	case MD_TEXT_NULLCHAR:
		/* U+FFFD replacement char, per CommonMark */
		emit(c, KMD_TEXT, c->attr_current, 0, "\xEF\xBF\xBD", 3);
		break;
	case MD_TEXT_NORMAL:
	case MD_TEXT_ENTITY:
	case MD_TEXT_HTML:
	case MD_TEXT_LATEXMATH:
	default:
		emit(c, KMD_TEXT, c->attr_current, 0, text, size);
		break;
	}
	return 0;
}

int kora_markdown_parse(const char *src, size_t len,
                        kora_md_emit_fn cb, void *user_data)
{
	if (!src || !cb) return -1;

	struct ctx c = { .cb = cb, .user_data = user_data };

	MD_PARSER parser = {
		.abi_version = 0,
		.flags = MD_DIALECT_GITHUB
		       | MD_FLAG_COLLAPSEWHITESPACE
		       | MD_FLAG_NOINDENTEDCODEBLOCKS
		       | MD_FLAG_NOHTML,
		.enter_block = on_enter_block,
		.leave_block = on_leave_block,
		.enter_span  = on_enter_span,
		.leave_span  = on_leave_span,
		.text        = on_text,
	};

	int rc = md_parse(src, (MD_SIZE)len, &parser, &c);
	free(c.code_line);
	return rc == 0 ? 0 : -1;
}
