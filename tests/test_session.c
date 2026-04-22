#include "test.h"
#include "client.h"   /* struct kora_message */
#include "session.h"

static void test_new_with_system_prompt_inserts_message0(void)
{
	TEST_BEGIN("kora_session_new with system prompt inserts message[0]");
	struct kora_session *s = kora_session_new("llama-3.2-3b", "you are helpful");
	ASSERT(s != NULL);
	EXPECT_EQ(s->n_msg, 1);
	EXPECT_STREQ(s->roles[0], "system");
	EXPECT_STREQ(s->contents[0], "you are helpful");
	EXPECT_STREQ(s->model, "llama-3.2-3b");
	kora_session_free(s);
	TEST_END();
}

static void test_new_without_system_prompt(void)
{
	TEST_BEGIN("kora_session_new without system prompt starts empty");
	struct kora_session *s = kora_session_new(NULL, NULL);
	ASSERT(s != NULL);
	EXPECT_EQ(s->n_msg, 0);
	EXPECT(s->model == NULL);
	EXPECT(s->system_prompt == NULL);
	kora_session_free(s);
	TEST_END();
}

static void test_add_appends_and_deep_copies(void)
{
	TEST_BEGIN("kora_session_add appends and deep-copies inputs");
	struct kora_session *s = kora_session_new(NULL, NULL);
	ASSERT(s != NULL);

	char role[] = "user";
	char content[] = "hello";
	EXPECT_EQ(kora_session_add(s, role, content), 0);

	/* mutating caller's buffer must not change the stored copy. */
	role[0] = 'X';
	content[0] = 'Y';
	EXPECT_STREQ(s->roles[0], "user");
	EXPECT_STREQ(s->contents[0], "hello");

	EXPECT_EQ(kora_session_add(s, "assistant", "hi there"), 0);
	EXPECT_EQ(s->n_msg, 2);

	kora_session_free(s);
	TEST_END();
}

static void test_clear_preserves_system_message(void)
{
	TEST_BEGIN("kora_session_clear keeps the system message at [0]");
	struct kora_session *s = kora_session_new(NULL, "keep me");
	ASSERT(s != NULL);
	kora_session_add(s, "user", "a");
	kora_session_add(s, "assistant", "b");
	kora_session_add(s, "user", "c");
	EXPECT_EQ(s->n_msg, 4);

	kora_session_clear(s);
	EXPECT_EQ(s->n_msg, 1);
	EXPECT_STREQ(s->roles[0], "system");
	EXPECT_STREQ(s->contents[0], "keep me");

	kora_session_free(s);
	TEST_END();
}

static void test_clear_without_system_empties_entirely(void)
{
	TEST_BEGIN("kora_session_clear drops everything when no system message");
	struct kora_session *s = kora_session_new(NULL, NULL);
	ASSERT(s != NULL);
	kora_session_add(s, "user", "hi");
	kora_session_add(s, "assistant", "hello");
	kora_session_clear(s);
	EXPECT_EQ(s->n_msg, 0);
	kora_session_free(s);
	TEST_END();
}

static void test_snapshot_roundtrip(void)
{
	TEST_BEGIN("kora_session_snapshot deep-copies message order and content");
	struct kora_session *s = kora_session_new(NULL, "sys");
	ASSERT(s != NULL);
	kora_session_add(s, "user", "one");
	kora_session_add(s, "assistant", "two");

	struct kora_message *snap = NULL;
	int n = kora_session_snapshot(s, &snap);
	EXPECT_EQ(n, 3);
	ASSERT(snap != NULL);
	EXPECT_STREQ(snap[0].role, "system");
	EXPECT_STREQ(snap[0].content, "sys");
	EXPECT_STREQ(snap[1].role, "user");
	EXPECT_STREQ(snap[1].content, "one");
	EXPECT_STREQ(snap[2].role, "assistant");
	EXPECT_STREQ(snap[2].content, "two");

	/* mutating session must not mutate the snapshot. */
	kora_session_clear(s);
	EXPECT_STREQ(snap[1].content, "one");

	kora_session_snapshot_free(snap, n);
	kora_session_free(s);
	TEST_END();
}

static void test_transcript_skips_system(void)
{
	TEST_BEGIN("kora_session_transcript omits the system message");
	struct kora_session *s = kora_session_new(NULL, "hidden");
	ASSERT(s != NULL);
	kora_session_add(s, "user", "a");
	kora_session_add(s, "assistant", "b");

	char *t = kora_session_transcript(s);
	ASSERT(t != NULL);
	EXPECT(strstr(t, "hidden") == NULL);
	EXPECT(strstr(t, "user: a") != NULL);
	EXPECT(strstr(t, "assistant: b") != NULL);
	free(t);
	kora_session_free(s);
	TEST_END();
}

static void test_set_model_replaces(void)
{
	TEST_BEGIN("kora_session_set_model replaces and frees old value");
	struct kora_session *s = kora_session_new("alpha", NULL);
	ASSERT(s != NULL);
	EXPECT_STREQ(s->model, "alpha");
	EXPECT_EQ(kora_session_set_model(s, "beta"), 0);
	EXPECT_STREQ(s->model, "beta");
	EXPECT_EQ(kora_session_set_model(s, NULL), 0);
	EXPECT(s->model == NULL);
	kora_session_free(s);
	TEST_END();
}

int main(void)
{
	test_new_with_system_prompt_inserts_message0();
	test_new_without_system_prompt();
	test_add_appends_and_deep_copies();
	test_clear_preserves_system_message();
	test_clear_without_system_empties_entirely();
	test_snapshot_roundtrip();
	test_transcript_skips_system();
	test_set_model_replaces();
	return TEST_REPORT();
}
