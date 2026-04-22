#include "test.h"
#include "client.h"

static void test_extracts_plain_content(void)
{
	TEST_BEGIN("extracts plain content from a minimal JSON object");
	char out[256];
	int n = kora_json_extract_content("{\"content\":\"hello\"}", out, sizeof out);
	EXPECT_EQ(n, 5);
	EXPECT_STREQ(out, "hello");
	TEST_END();
}

static void test_unescapes_common_escapes(void)
{
	TEST_BEGIN("unescapes \\n \\t \\r \\\" \\\\");
	char out[256];
	int n = kora_json_extract_content(
		"{\"content\":\"a\\nb\\tc\\r\\\"d\\\\e\"}",
		out, sizeof out);
	EXPECT(n > 0);
	EXPECT_STREQ(out, "a\nb\tc\r\"d\\e");
	TEST_END();
}

static void test_unescapes_unicode_bmp(void)
{
	TEST_BEGIN("unescapes a BMP \\u code point to UTF-8 bytes");
	char out[32];
	/* U+00E9 é → 0xC3 0xA9 */
	int n = kora_json_extract_content("{\"content\":\"caf\\u00e9\"}",
	                                  out, sizeof out);
	EXPECT_EQ(n, 5);
	EXPECT(memcmp(out, "caf\xc3\xa9", 5) == 0);
	TEST_END();
}

static void test_missing_content_returns_minus_one(void)
{
	TEST_BEGIN("missing \"content\" field returns -1");
	char out[32];
	int n = kora_json_extract_content("{\"other\":\"x\"}", out, sizeof out);
	EXPECT_EQ(n, -1);
	TEST_END();
}

static void test_extracts_from_delta_payload(void)
{
	TEST_BEGIN("extracts from OpenAI-compat streaming delta payload");
	const char *json =
		"{\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":\"hi\"},"
		"\"finish_reason\":null}]}";
	char out[32];
	int n = kora_json_extract_content(json, out, sizeof out);
	EXPECT_EQ(n, 2);
	EXPECT_STREQ(out, "hi");
	TEST_END();
}

static void test_empty_content(void)
{
	TEST_BEGIN("empty content string returns 0 and null-terminates out");
	char out[16];
	out[0] = 'X';
	int n = kora_json_extract_content("{\"content\":\"\"}", out, sizeof out);
	EXPECT_EQ(n, 0);
	EXPECT(out[0] == '\0');
	TEST_END();
}

int main(void)
{
	test_extracts_plain_content();
	test_unescapes_common_escapes();
	test_unescapes_unicode_bmp();
	test_missing_content_returns_minus_one();
	test_extracts_from_delta_payload();
	test_empty_content();
	return TEST_REPORT();
}
