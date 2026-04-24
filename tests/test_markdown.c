#include "test.h"
#include "markdown.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test harness: accumulate every emit call into a linear log. Each event
   is serialised as a human-readable line so asserts read naturally.
   Format:
       TEXT|attrs=N|"bytes"
       PARA
       HARD_BR
       H_BEGIN|lvl=N
       H_END
       CODE_BEGIN|lang="..."
       CODE_LINE|"..."
       CODE_END
       LI_BEGIN|lvl=N
       LI_END
       QUOTE_BEGIN
       QUOTE_END
*/

struct log_buf {
	char  *data;
	size_t len;
	size_t cap;
};

static void lb_append(struct log_buf *b, const char *s)
{
	size_t n = strlen(s);
	if (b->len + n + 1 >= b->cap) {
		size_t nc = b->cap ? b->cap * 2 : 512;
		while (nc < b->len + n + 1) nc *= 2;
		b->data = realloc(b->data, nc);
		b->cap = nc;
	}
	memcpy(b->data + b->len, s, n);
	b->len += n;
	b->data[b->len] = '\0';
}

static void lb_appendf(struct log_buf *b, const char *fmt, ...)
{
	char tmp[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(tmp, sizeof tmp, fmt, ap);
	va_end(ap);
	lb_append(b, tmp);
}

static void emit_cb(int kind, int attrs, int level,
                    const char *text, size_t len, void *user)
{
	struct log_buf *b = user;
	switch (kind) {
	case KMD_TEXT:
		lb_appendf(b, "TEXT|attrs=%d|\"%.*s\"\n",
		           attrs, (int)len, text ? text : "");
		break;
	case KMD_PARAGRAPH_BREAK:
		lb_append(b, "PARA\n");
		break;
	case KMD_HARD_BREAK:
		lb_append(b, "HARD_BR\n");
		break;
	case KMD_HEADING_BEGIN:
		lb_appendf(b, "H_BEGIN|lvl=%d\n", level);
		break;
	case KMD_HEADING_END:
		lb_append(b, "H_END\n");
		break;
	case KMD_CODE_BLOCK_BEGIN:
		lb_appendf(b, "CODE_BEGIN|lang=\"%.*s\"\n",
		           (int)len, text ? text : "");
		break;
	case KMD_CODE_BLOCK_LINE:
		lb_appendf(b, "CODE_LINE|\"%.*s\"\n",
		           (int)len, text ? text : "");
		break;
	case KMD_CODE_BLOCK_END:
		lb_append(b, "CODE_END\n");
		break;
	case KMD_LIST_ITEM_BEGIN:
		lb_appendf(b, "LI_BEGIN|lvl=%d\n", level);
		break;
	case KMD_LIST_ITEM_END:
		lb_append(b, "LI_END\n");
		break;
	case KMD_BLOCKQUOTE_BEGIN:
		lb_append(b, "QUOTE_BEGIN\n");
		break;
	case KMD_BLOCKQUOTE_END:
		lb_append(b, "QUOTE_END\n");
		break;
	case KMD_TABLE_BEGIN:
		lb_append(b, "TABLE_BEGIN\n");
		break;
	case KMD_TABLE_END:
		lb_append(b, "TABLE_END\n");
		break;
	case KMD_TABLE_ROW_BEGIN:
		lb_append(b, "ROW_BEGIN\n");
		break;
	case KMD_TABLE_ROW_END:
		lb_append(b, "ROW_END\n");
		break;
	case KMD_TABLE_CELL_BEGIN:
		lb_appendf(b, "CELL_BEGIN|hdr=%d\n", level);
		break;
	case KMD_TABLE_CELL_END:
		lb_append(b, "CELL_END\n");
		break;
	}
}

static char *render(const char *src)
{
	struct log_buf b = {0};
	int rc = kora_markdown_parse(src, strlen(src), emit_cb, &b);
	if (rc != 0) {
		free(b.data);
		return NULL;
	}
	if (!b.data) {
		b.data = strdup("");
	}
	return b.data;
}

static void test_plain_text(void)
{
	TEST_BEGIN("plain text becomes a single TEXT event with no attrs");
	char *out = render("hello world");
	ASSERT(out != NULL);
	EXPECT_STREQ(out, "TEXT|attrs=0|\"hello world\"\n");
	free(out);
	TEST_END();
}

static void test_bold_italic_code_inline(void)
{
	TEST_BEGIN("bold / italic / inline code set attr bits");
	char *out = render("**bold** *italic* `code`");
	ASSERT(out != NULL);
	/* attr values: BOLD=1, ITALIC=2, CODE_INLINE=4 */
	EXPECT(strstr(out, "TEXT|attrs=1|\"bold\"")       != NULL);
	EXPECT(strstr(out, "TEXT|attrs=2|\"italic\"")     != NULL);
	EXPECT(strstr(out, "TEXT|attrs=4|\"code\"")       != NULL);
	free(out);
	TEST_END();
}

static void test_heading_levels(void)
{
	TEST_BEGIN("headings emit BEGIN/END with correct level");
	char *out = render("# H1\n\n## H2\n\n### H3\n");
	ASSERT(out != NULL);
	EXPECT(strstr(out, "H_BEGIN|lvl=1") != NULL);
	EXPECT(strstr(out, "H_BEGIN|lvl=2") != NULL);
	EXPECT(strstr(out, "H_BEGIN|lvl=3") != NULL);
	EXPECT(strstr(out, "TEXT|attrs=0|\"H1\"") != NULL);
	free(out);
	TEST_END();
}

static void test_fenced_code_block_with_language(void)
{
	TEST_BEGIN("fenced code block emits BEGIN with lang + per-line events");
	char *out = render("```python\ndef foo():\n    pass\n```\n");
	ASSERT(out != NULL);
	EXPECT(strstr(out, "CODE_BEGIN|lang=\"python\"") != NULL);
	EXPECT(strstr(out, "CODE_LINE|\"def foo():\"")    != NULL);
	EXPECT(strstr(out, "CODE_LINE|\"    pass\"")      != NULL);
	EXPECT(strstr(out, "CODE_END")                     != NULL);
	free(out);
	TEST_END();
}

static void test_fenced_code_block_no_language(void)
{
	TEST_BEGIN("fenced code block without language has empty lang field");
	char *out = render("```\nraw\n```\n");
	ASSERT(out != NULL);
	EXPECT(strstr(out, "CODE_BEGIN|lang=\"\"") != NULL);
	EXPECT(strstr(out, "CODE_LINE|\"raw\"")    != NULL);
	free(out);
	TEST_END();
}

static void test_paragraph_break_between_blocks(void)
{
	TEST_BEGIN("consecutive paragraphs get a PARA event between them");
	char *out = render("first\n\nsecond\n");
	ASSERT(out != NULL);
	EXPECT(strstr(out, "TEXT|attrs=0|\"first\"")  != NULL);
	EXPECT(strstr(out, "PARA")                    != NULL);
	EXPECT(strstr(out, "TEXT|attrs=0|\"second\"") != NULL);
	free(out);
	TEST_END();
}

static void test_no_leading_or_trailing_para_break(void)
{
	TEST_BEGIN("single paragraph has no leading/trailing PARA");
	char *out = render("just one paragraph");
	ASSERT(out != NULL);
	/* exactly one TEXT event, no PARA */
	EXPECT(strstr(out, "PARA") == NULL);
	free(out);
	TEST_END();
}

static void test_soft_break_collapses_to_space(void)
{
	TEST_BEGIN("soft line breaks in a paragraph collapse to a single space");
	char *out = render("line one\nline two\n");
	ASSERT(out != NULL);
	EXPECT(strstr(out, "TEXT|attrs=0|\" \"") != NULL);  /* the softbr space */
	EXPECT(strstr(out, "HARD_BR")            == NULL);
	free(out);
	TEST_END();
}

static void test_list_items(void)
{
	TEST_BEGIN("bullet list emits LI_BEGIN/END around each item");
	char *out = render("- first\n- second\n- third\n");
	ASSERT(out != NULL);
	/* three LI_BEGIN / LI_END pairs */
	int begins = 0, ends = 0;
	for (const char *p = out; (p = strstr(p, "LI_BEGIN|lvl=1")); p++) begins++;
	for (const char *p = out; (p = strstr(p, "LI_END"));        p++) ends++;
	EXPECT_EQ(begins, 3);
	EXPECT_EQ(ends,   3);
	free(out);
	TEST_END();
}

static void test_nested_list_depth(void)
{
	TEST_BEGIN("nested list items report nesting depth");
	char *out = render("- outer\n  - inner\n");
	ASSERT(out != NULL);
	EXPECT(strstr(out, "LI_BEGIN|lvl=1") != NULL);
	EXPECT(strstr(out, "LI_BEGIN|lvl=2") != NULL);
	free(out);
	TEST_END();
}

static void test_blockquote(void)
{
	TEST_BEGIN("blockquote emits QUOTE_BEGIN/END around contents");
	char *out = render("> quoted\n");
	ASSERT(out != NULL);
	EXPECT(strstr(out, "QUOTE_BEGIN") != NULL);
	EXPECT(strstr(out, "QUOTE_END")   != NULL);
	EXPECT(strstr(out, "TEXT|attrs=0|\"quoted\"") != NULL);
	free(out);
	TEST_END();
}

static void test_bold_inside_list(void)
{
	TEST_BEGIN("inline attrs work inside list items");
	char *out = render("- **important** note\n");
	ASSERT(out != NULL);
	EXPECT(strstr(out, "LI_BEGIN|lvl=1")                 != NULL);
	EXPECT(strstr(out, "TEXT|attrs=1|\"important\"")     != NULL);
	EXPECT(strstr(out, "TEXT|attrs=0|\" note\"")         != NULL);
	free(out);
	TEST_END();
}

static void test_table_basic(void)
{
	TEST_BEGIN("table emits TABLE/ROW/CELL events with header distinction");
	char *out = render(
		"| Col 1 | Col 2 |\n"
		"| ----- | ----- |\n"
		"| A     | B     |\n"
		"| C     | D     |\n");
	ASSERT(out != NULL);
	EXPECT(strstr(out, "TABLE_BEGIN")         != NULL);
	EXPECT(strstr(out, "TABLE_END")           != NULL);
	EXPECT(strstr(out, "CELL_BEGIN|hdr=1")    != NULL);  /* header row */
	EXPECT(strstr(out, "CELL_BEGIN|hdr=0")    != NULL);  /* body rows */
	EXPECT(strstr(out, "TEXT|attrs=0|\"Col 1\"") != NULL);
	EXPECT(strstr(out, "TEXT|attrs=0|\"A\"")  != NULL);
	/* no PARA event should leak inside the table */
	const char *tbl_start = strstr(out, "TABLE_BEGIN");
	const char *tbl_end   = strstr(out, "TABLE_END");
	ASSERT(tbl_start && tbl_end && tbl_end > tbl_start);
	/* search for "PARA\n" inside the table span */
	int has_inner_para = 0;
	for (const char *p = tbl_start; p < tbl_end; p++) {
		if (strncmp(p, "PARA\n", 5) == 0) { has_inner_para = 1; break; }
	}
	EXPECT_EQ(has_inner_para, 0);
	free(out);
	TEST_END();
}

static void test_empty_input(void)
{
	TEST_BEGIN("empty input produces no events");
	char *out = render("");
	ASSERT(out != NULL);
	EXPECT_STREQ(out, "");
	free(out);
	TEST_END();
}

static void test_null_input(void)
{
	TEST_BEGIN("NULL src returns -1");
	int rc = kora_markdown_parse(NULL, 0, emit_cb, NULL);
	EXPECT_EQ(rc, -1);
	TEST_END();
}

int main(void)
{
	test_plain_text();
	test_bold_italic_code_inline();
	test_heading_levels();
	test_fenced_code_block_with_language();
	test_fenced_code_block_no_language();
	test_paragraph_break_between_blocks();
	test_no_leading_or_trailing_para_break();
	test_soft_break_collapses_to_space();
	test_list_items();
	test_nested_list_depth();
	test_blockquote();
	test_bold_inside_list();
	test_table_basic();
	test_empty_input();
	test_null_input();
	return TEST_REPORT();
}
