#ifndef KORA_MARKDOWN_H
#define KORA_MARKDOWN_H

#include <stddef.h>

/* Callback-based markdown renderer. md4c parses CommonMark/GFM, we flatten
   its SAX events into a smaller "presentation" event stream that the chat
   pad can consume directly. Two upsides of the callback model over a span
   list:

     1. No intermediate allocation — the TUI renderer applies ncurses attrs
        and calls waddnstr() inline.
     2. Code-block syntax highlighting slots in later without touching the
        markdown layer: the consumer sees KMD_CODE_BLOCK_BEGIN → multiple
        KMD_CODE_BLOCK_LINE → KMD_CODE_BLOCK_END and can tokenize the lines
        on its own before writing to the pad.
*/

/* attribute bits — combined on KMD_TEXT events */
#define KMD_ATTR_BOLD        0x01
#define KMD_ATTR_ITALIC      0x02
#define KMD_ATTR_CODE_INLINE 0x04

enum kora_md_kind {
	KMD_TEXT,              /* inline run; .attrs set; .text/.len hold bytes */
	KMD_HARD_BREAK,        /* explicit <br> / double-space-newline */
	KMD_PARAGRAPH_BREAK,   /* blank line between blocks */
	KMD_HEADING_BEGIN,     /* .level = 1..6 */
	KMD_HEADING_END,
	KMD_CODE_BLOCK_BEGIN,  /* .text/.len = language tag (may be empty) */
	KMD_CODE_BLOCK_LINE,   /* one line of code, no trailing newline */
	KMD_CODE_BLOCK_END,
	KMD_LIST_ITEM_BEGIN,   /* .level = nesting depth (1-based) */
	KMD_LIST_ITEM_END,
	KMD_BLOCKQUOTE_BEGIN,
	KMD_BLOCKQUOTE_END,
	KMD_TABLE_BEGIN,
	KMD_TABLE_END,
	KMD_TABLE_ROW_BEGIN,
	KMD_TABLE_ROW_END,
	KMD_TABLE_CELL_BEGIN,  /* .level = 1 for header cells, 0 for body */
	KMD_TABLE_CELL_END,
};

typedef void (*kora_md_emit_fn)(
	int         kind,
	int         attrs,
	int         level,
	const char *text,
	size_t      len,
	void       *user_data);

/* Parse `src` of `len` bytes and invoke `cb` for each presentation event.
   Returns 0 on success, -1 if md4c returned non-zero. */
int kora_markdown_parse(const char *src, size_t len,
                        kora_md_emit_fn cb, void *user_data);

#endif
