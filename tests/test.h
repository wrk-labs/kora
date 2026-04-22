#ifndef KORA_TEST_H
#define KORA_TEST_H

/* minimal unit-test harness. each test binary has its own main() that
   calls test functions; each test function uses EXPECT / ASSERT to record
   failures. main() returns TEST_REPORT()'s exit code. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int   kora_test_pass = 0;
static int   kora_test_fail = 0;
static const char *kora_test_name = "?";

#define TEST_BEGIN(name) do { kora_test_name = (name); \
	fprintf(stderr, "  RUN  %s\n", kora_test_name); } while (0)

#define TEST_END() do { \
	fprintf(stderr, "       %s: %d passed, %d failed\n\n", \
	        kora_test_name, kora_test_pass, kora_test_fail); \
} while (0)

#define EXPECT(cond) do { \
	if (cond) { kora_test_pass++; } \
	else { kora_test_fail++; \
	       fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define EXPECT_STREQ(a, b) do { \
	const char *_a = (a), *_b = (b); \
	if (_a && _b && strcmp(_a, _b) == 0) { kora_test_pass++; } \
	else { kora_test_fail++; \
	       fprintf(stderr, "  FAIL %s:%d: expected \"%s\", got \"%s\"\n", \
	               __FILE__, __LINE__, _b ? _b : "(null)", _a ? _a : "(null)"); } \
} while (0)

#define EXPECT_EQ(a, b) do { \
	long _a = (long)(a), _b = (long)(b); \
	if (_a == _b) { kora_test_pass++; } \
	else { kora_test_fail++; \
	       fprintf(stderr, "  FAIL %s:%d: expected %ld, got %ld\n", \
	               __FILE__, __LINE__, _b, _a); } \
} while (0)

#define ASSERT(cond) do { \
	if (!(cond)) { kora_test_fail++; \
	               fprintf(stderr, "  FATAL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	               return; } \
	else { kora_test_pass++; } \
} while (0)

#define TEST_REPORT() ( \
	fprintf(stderr, "=== %d passed, %d failed ===\n", kora_test_pass, kora_test_fail), \
	kora_test_fail == 0 ? 0 : 1 \
)

#endif
