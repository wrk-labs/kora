#include "test.h"
#include "prompt.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static void test_null_template(void)
{
	TEST_BEGIN("NULL template returns NULL");
	EXPECT(kora_prompt_render(NULL, "m", 4096) == NULL);
	TEST_END();
}

static void test_empty_template(void)
{
	TEST_BEGIN("empty template renders to empty string");
	char *out = kora_prompt_render("", "m", 4096);
	ASSERT(out != NULL);
	EXPECT_STREQ(out, "");
	free(out);
	TEST_END();
}

static void test_literal_passthrough(void)
{
	TEST_BEGIN("template with no placeholders is copied verbatim");
	char *out = kora_prompt_render("hello world.", "m", 4096);
	ASSERT(out != NULL);
	EXPECT_STREQ(out, "hello world.");
	free(out);
	TEST_END();
}

static void test_model_substitution(void)
{
	TEST_BEGIN("{model} substitutes the passed model alias");
	char *out = kora_prompt_render("you are {model}", "qwen-coder-1.5b", 0);
	ASSERT(out != NULL);
	EXPECT_STREQ(out, "you are qwen-coder-1.5b");
	free(out);
	TEST_END();
}

static void test_model_fallback(void)
{
	TEST_BEGIN("{model} falls back to (unknown) when NULL or empty");
	char *a = kora_prompt_render("m={model}", NULL, 0);
	char *b = kora_prompt_render("m={model}", "",   0);
	ASSERT(a != NULL); ASSERT(b != NULL);
	EXPECT_STREQ(a, "m=(unknown)");
	EXPECT_STREQ(b, "m=(unknown)");
	free(a); free(b);
	TEST_END();
}

static void test_ctx_substitution(void)
{
	TEST_BEGIN("{ctx} renders as decimal or (unknown) for non-positive");
	char *a = kora_prompt_render("ctx={ctx}", "m", 8192);
	char *b = kora_prompt_render("ctx={ctx}", "m", 0);
	char *c = kora_prompt_render("ctx={ctx}", "m", -1);
	ASSERT(a != NULL); ASSERT(b != NULL); ASSERT(c != NULL);
	EXPECT_STREQ(a, "ctx=8192");
	EXPECT_STREQ(b, "ctx=(unknown)");
	EXPECT_STREQ(c, "ctx=(unknown)");
	free(a); free(b); free(c);
	TEST_END();
}

static void test_unknown_placeholder_passthrough(void)
{
	TEST_BEGIN("unknown placeholders are left verbatim so typos are visible");
	char *out = kora_prompt_render("{unknown} {model}", "x", 4096);
	ASSERT(out != NULL);
	EXPECT_STREQ(out, "{unknown} x");
	free(out);
	TEST_END();
}

static void test_malformed_braces_passthrough(void)
{
	TEST_BEGIN("braces with no close, caps, or spaces stay literal");
	char *a = kora_prompt_render("{unterminated", "m", 0);
	char *b = kora_prompt_render("{MODEL}", "m", 0);
	char *c = kora_prompt_render("{foo bar}", "m", 0);
	char *d = kora_prompt_render("{}", "m", 0);
	ASSERT(a && b && c && d);
	EXPECT_STREQ(a, "{unterminated");
	EXPECT_STREQ(b, "{MODEL}");
	EXPECT_STREQ(c, "{foo bar}");
	EXPECT_STREQ(d, "{}");
	free(a); free(b); free(c); free(d);
	TEST_END();
}

static void test_date_looks_sane(void)
{
	TEST_BEGIN("{date} renders to a non-empty string containing the year");
	char *out = kora_prompt_render("{date}", "m", 0);
	ASSERT(out != NULL);
	EXPECT(strlen(out) > 0);

	/* sanity: output should contain the current year as 4 digits. */
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	char year[16];
	snprintf(year, sizeof year, "%d", tm.tm_year + 1900);
	EXPECT(strstr(out, year) != NULL);
	free(out);
	TEST_END();
}

static void test_platform_lowercased(void)
{
	TEST_BEGIN("{platform} renders as lowercase");
	char *out = kora_prompt_render("{platform}", "m", 0);
	ASSERT(out != NULL);
	for (const char *p = out; *p; p++) {
		EXPECT(!(*p >= 'A' && *p <= 'Z'));
	}
	free(out);
	TEST_END();
}

static void test_multiple_occurrences(void)
{
	TEST_BEGIN("same placeholder can appear multiple times");
	char *out = kora_prompt_render("{model}/{model}/{model}", "m", 0);
	ASSERT(out != NULL);
	EXPECT_STREQ(out, "m/m/m");
	free(out);
	TEST_END();
}

int main(void)
{
	test_null_template();
	test_empty_template();
	test_literal_passthrough();
	test_model_substitution();
	test_model_fallback();
	test_ctx_substitution();
	test_unknown_placeholder_passthrough();
	test_malformed_braces_passthrough();
	test_date_looks_sane();
	test_platform_lowercased();
	test_multiple_occurrences();
	return TEST_REPORT();
}
