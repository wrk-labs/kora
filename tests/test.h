#ifndef KORA_TEST_H
#define KORA_TEST_H

/* minimal unit-test harness. each test binary has its own main() that
   calls test functions; each test function uses EXPECT / ASSERT to record
   failures. main() returns TEST_REPORT()'s exit code.

   output is silent on success — TEST_REPORT prints a single summary line.
   failures still print the file:line + assertion at the point of failure,
   so a failed run is self-explanatory. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int   kora_test_pass = 0;
static int   kora_test_fail = 0;
static int   kora_test_groups = 0;
static const char *kora_test_name = "?";

#define TEST_BEGIN(name) do { kora_test_name = (name); \
                              kora_test_groups++; } while (0)

#define TEST_END() ((void)0)

#define EXPECT(cond) do { \
	if (cond) { kora_test_pass++; } \
	else { kora_test_fail++; \
	       fprintf(stderr, "  FAIL %s:%d in \"%s\": %s\n", \
	               __FILE__, __LINE__, kora_test_name, #cond); } \
} while (0)

#define EXPECT_STREQ(a, b) do { \
	const char *_a = (a), *_b = (b); \
	if (_a && _b && strcmp(_a, _b) == 0) { kora_test_pass++; } \
	else { kora_test_fail++; \
	       fprintf(stderr, "  FAIL %s:%d in \"%s\": expected \"%s\", got \"%s\"\n", \
	               __FILE__, __LINE__, kora_test_name, \
	               _b ? _b : "(null)", _a ? _a : "(null)"); } \
} while (0)

#define EXPECT_EQ(a, b) do { \
	long _a = (long)(a), _b = (long)(b); \
	if (_a == _b) { kora_test_pass++; } \
	else { kora_test_fail++; \
	       fprintf(stderr, "  FAIL %s:%d in \"%s\": expected %ld, got %ld\n", \
	               __FILE__, __LINE__, kora_test_name, _b, _a); } \
} while (0)

#define ASSERT(cond) do { \
	if (!(cond)) { kora_test_fail++; \
	               fprintf(stderr, "  FATAL %s:%d in \"%s\": %s\n", \
	                       __FILE__, __LINE__, kora_test_name, #cond); \
	               return; } \
	else { kora_test_pass++; } \
} while (0)

#define TEST_REPORT() ( \
	kora_test_fail == 0 \
		? (printf("%d checks across %d tests\n", \
		          kora_test_pass, kora_test_groups), 0) \
		: (fprintf(stderr, "  %d/%d checks FAILED across %d tests\n", \
		           kora_test_fail, kora_test_pass + kora_test_fail, \
		           kora_test_groups), 1) \
)

#endif
