#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/agent/parser.h"

#define C_GREEN  "\x1b[32m"
#define C_RED    "\x1b[31m"
#define C_DIM    "\x1b[2m"
#define C_BOLD   "\x1b[1m"
#define C_RESET  "\x1b[0m"

static int total_pass = 0, total_fail = 0;
static int case_pass, case_fail;
static const char *case_name = "?";
static int verbose = 0;

/* failure log: kept lean — first 32 failures across all cases */
static struct { const char *case_name; const char *assertion; int line; } failures[32];
static int n_failures = 0;

#define CHECK(expr) do { \
	if (expr) { \
		case_pass++; \
		if (verbose) printf("    " C_GREEN "ok  " C_RESET "%s\n", #expr); \
	} else { \
		case_fail++; \
		if (n_failures < 32) { \
			failures[n_failures].case_name = case_name; \
			failures[n_failures].assertion = #expr; \
			failures[n_failures].line = __LINE__; \
			n_failures++; \
		} \
	} \
} while (0)

static void case_begin(const char *name)
{
	case_pass = case_fail = 0;
	case_name = name;
}

static void case_end(void)
{
	int total = case_pass + case_fail;
	if (case_fail == 0) {
		printf("  " C_GREEN "ok  " C_RESET "%-30s " C_DIM "%d/%d" C_RESET "\n",
			case_name, case_pass, total);
	} else {
		printf("  " C_RED "FAIL" C_RESET " %-30s " C_DIM "%d/%d" C_RESET "\n",
			case_name, case_pass, total);
	}
	total_pass += case_pass;
	total_fail += case_fail;
}

#define TEST(name, body) do { \
	case_begin(name); \
	body; \
	case_end(); \
} while (0)

static void feed_chunked(struct kora_parser *p, const char *full, int chunk_size)
{
	int i, len = (int)strlen(full);
	for (i = 0; i < len; i += chunk_size) {
		int n = (i + chunk_size > len) ? (len - i) : chunk_size;
		if (kora_parser_feed(p, full + i, n))
			return;  /* DONE */
	}
}

static void test_complete_in_one_feed(void)
{
	case_begin("complete in one feed");
	struct kora_parser p;
	kora_parser_init(&p);

	const char *input = "thinking...\n<tool_call>{\"name\":\"read\",\"arguments\":{\"file_path\":\"/x\"}}</tool_call>";
	int done = kora_parser_feed(&p, input, (int)strlen(input));

	CHECK(done == 1);
	CHECK(p.state == KORA_PARSE_DONE);
	CHECK(p.call_body != NULL);
	CHECK(strcmp(p.call_body, "{\"name\":\"read\",\"arguments\":{\"file_path\":\"/x\"}}") == 0);

	const char *text = kora_parser_text(&p);
	CHECK(text != NULL && strncmp(text, "thinking...\n", 12) == 0);

	kora_parser_free(&p);
	case_end();
}

static void test_split_across_chunks(void)
{
	case_begin("split across chunks");
	const char *input = "before<tool_call>{\"name\":\"bash\",\"arguments\":{\"command\":\"ls\"}}</tool_call>";
	int sizes[] = { 1, 2, 3, 5, 7, 13, 100 };
	int i;
	for (i = 0; i < 7; i++) {
		struct kora_parser p;
		kora_parser_init(&p);
		feed_chunked(&p, input, sizes[i]);
		CHECK(p.state == KORA_PARSE_DONE);
		CHECK(p.call_body != NULL);
		CHECK(strcmp(p.call_body, "{\"name\":\"bash\",\"arguments\":{\"command\":\"ls\"}}") == 0);
		kora_parser_free(&p);
	}
	case_end();
}

static void test_no_tool_call(void)
{
	case_begin("no tool call");
	struct kora_parser p;
	kora_parser_init(&p);

	const char *input = "just a plain assistant response, no tools here.";
	int done = kora_parser_feed(&p, input, (int)strlen(input));

	CHECK(done == 0);
	CHECK(p.state == KORA_PARSE_TEXT);
	CHECK(p.call_body == NULL);

	const char *text = kora_parser_text(&p);
	CHECK(strcmp(text, input) == 0);

	kora_parser_free(&p);
	case_end();
}

static void test_open_without_close(void)
{
	case_begin("open without close, then close later");
	struct kora_parser p;
	kora_parser_init(&p);

	const char *input = "starting<tool_call>{\"name\":\"x\"";
	int done = kora_parser_feed(&p, input, (int)strlen(input));

	CHECK(done == 0);
	CHECK(p.state == KORA_PARSE_INSIDE);

	/* now feed the close */
	const char *more = "}</tool_call>";
	done = kora_parser_feed(&p, more, (int)strlen(more));
	CHECK(done == 1);
	CHECK(p.state == KORA_PARSE_DONE);
	CHECK(strcmp(p.call_body, "{\"name\":\"x\"}") == 0);

	kora_parser_free(&p);
	case_end();
}

static void test_reset(void)
{
	case_begin("reset clears state");
	struct kora_parser p;
	kora_parser_init(&p);

	const char *first = "<tool_call>{\"name\":\"a\"}</tool_call>";
	kora_parser_feed(&p, first, (int)strlen(first));
	CHECK(p.state == KORA_PARSE_DONE);

	kora_parser_reset(&p);
	CHECK(p.state == KORA_PARSE_TEXT);
	CHECK(p.call_body == NULL);
	CHECK(p.buf_len == 0);

	const char *second = "fresh text";
	kora_parser_feed(&p, second, (int)strlen(second));
	CHECK(p.state == KORA_PARSE_TEXT);
	CHECK(strcmp(kora_parser_text(&p), "fresh text") == 0);

	kora_parser_free(&p);
	case_end();
}

static void test_byte_at_a_time(void)
{
	case_begin("byte at a time");
	const char *input = "x<tool_call>{}</tool_call>y";
	struct kora_parser p;
	kora_parser_init(&p);
	int i, len = (int)strlen(input), done = 0;
	for (i = 0; i < len && !done; i++) {
		done = kora_parser_feed(&p, input + i, 1);
	}
	CHECK(done == 1);
	CHECK(p.state == KORA_PARSE_DONE);
	CHECK(strcmp(p.call_body, "{}") == 0);
	kora_parser_free(&p);
	case_end();
}

static void test_finalize_inside(void)
{
	case_begin("finalize inside (EOG before close marker)");
	struct kora_parser p;
	kora_parser_init(&p);

	/* model emitted open tag + body but never the close tag (EOG fired) */
	const char *input = "<tool_call>\n{\"name\":\"read\",\"arguments\":{\"file_path\":\"/x\"}}\n";
	kora_parser_feed(&p, input, (int)strlen(input));
	CHECK(p.state == KORA_PARSE_INSIDE);

	int closed = kora_parser_finalize(&p);
	CHECK(closed == 1);
	CHECK(p.state == KORA_PARSE_DONE);
	CHECK(p.call_body != NULL);
	CHECK(strcmp(p.call_body, "{\"name\":\"read\",\"arguments\":{\"file_path\":\"/x\"}}") == 0);

	kora_parser_free(&p);
	case_end();
}

static void test_finalize_text_state_noop(void)
{
	case_begin("finalize is no-op in TEXT state");
	struct kora_parser p;
	kora_parser_init(&p);
	kora_parser_feed(&p, "plain text only", 15);
	int closed = kora_parser_finalize(&p);
	CHECK(closed == 0);
	CHECK(p.state == KORA_PARSE_TEXT);
	CHECK(p.call_body == NULL);
	kora_parser_free(&p);
	case_end();
}

int main(void)
{
	if (getenv("V") && getenv("V")[0] == '1')
		verbose = 1;

	printf(C_BOLD "c/test_parser" C_RESET "\n");

	test_complete_in_one_feed();
	test_split_across_chunks();
	test_no_tool_call();
	test_open_without_close();
	test_reset();
	test_byte_at_a_time();
	test_finalize_inside();
	test_finalize_text_state_noop();

	if (n_failures > 0) {
		printf("\n" C_BOLD "failures:" C_RESET "\n");
		int i;
		for (i = 0; i < n_failures; i++) {
			printf("  " C_RED "✗" C_RESET " %s\n", failures[i].case_name);
			printf("    " C_DIM "%s:%d  %s" C_RESET "\n",
				"tests/c/test_parser.c", failures[i].line, failures[i].assertion);
		}
	}

	printf("\n");
	if (total_fail == 0) {
		printf(C_GREEN "%d assertions passed" C_RESET " across 8 cases\n", total_pass);
	} else {
		printf(C_RED "%d passed, %d failed" C_RESET " of %d assertions\n",
			total_pass, total_fail, total_pass + total_fail);
	}
	return total_fail == 0 ? 0 : 1;
}
