#include "test.h"
#include "db.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* point $HOME at a fresh temp directory so db_open writes to a sandbox
   instead of the developer's real ~/.kora. each test binary owns its
   sandbox for the duration of main(). */
static char tmp_home[512];

static int setup_tmp_home(void)
{
	char tpl[] = "/tmp/kora-test-XXXXXX";
	char *p = mkdtemp(tpl);
	if (!p) return -1;
	snprintf(tmp_home, sizeof tmp_home, "%s", p);
	setenv("HOME", tmp_home, 1);
	return kora_init_dirs();
}

static void teardown_tmp_home(void)
{
	/* best-effort cleanup; tests are sandboxed under /tmp anyway. */
	char cmd[600];
	snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmp_home);
	(void)system(cmd);
}

static void test_open_close_idempotent(void)
{
	TEST_BEGIN("db_open / db_close cycle works and creates kora.db");
	EXPECT_EQ(db_open(), 0);
	db_close();

	char path[700];
	snprintf(path, sizeof path, "%s/.kora/kora.db", tmp_home);
	struct stat st;
	EXPECT_EQ(stat(path, &st), 0);
	EXPECT(st.st_size > 0);

	/* reopen should succeed on the existing file. */
	EXPECT_EQ(db_open(), 0);
	db_close();
	TEST_END();
}

static void test_session_roundtrip_preserves_messages(void)
{
	TEST_BEGIN("messages survive close+reopen byte-exact, including UTF-8");
	ASSERT(db_open() == 0);

	int sid = db_session_create("chat", "llama-3.2-3b", "/tmp/cwd");
	EXPECT(sid > 0);

	const char *weird = "café\n\"quoted\"\t<tag>\xF0\x9F\x91\x8B";
	db_message_add(sid, 0, "user",      weird,       "llama-3.2-3b", 1);
	db_message_add(sid, 1, "assistant", "ok, got it", "llama-3.2-3b", 1);

	db_close();
	ASSERT(db_open() == 0);

	struct db_message *msgs = NULL;
	int n = db_messages_load(sid, &msgs);
	EXPECT_EQ(n, 2);
	ASSERT(msgs != NULL);

	EXPECT_STREQ(msgs[0].role, "user");
	EXPECT_STREQ(msgs[0].content, weird);
	EXPECT_EQ(msgs[0].seq, 0);
	EXPECT_EQ(msgs[0].llm_use, 1);

	EXPECT_STREQ(msgs[1].role, "assistant");
	EXPECT_STREQ(msgs[1].content, "ok, got it");
	EXPECT_EQ(msgs[1].seq, 1);

	db_messages_free(msgs, n);
	db_close();
	TEST_END();
}

static void test_session_delete_cascades_to_messages(void)
{
	TEST_BEGIN("db_session_delete also removes messages");
	ASSERT(db_open() == 0);
	int sid = db_session_create("chat", "m", NULL);
	EXPECT(sid > 0);
	db_message_add(sid, 0, "user", "x", NULL, 1);
	db_message_add(sid, 1, "assistant", "y", NULL, 1);

	db_session_delete(sid);

	struct db_message *msgs = NULL;
	int n = db_messages_load(sid, &msgs);
	EXPECT_EQ(n, 0);
	db_messages_free(msgs, n);

	struct db_session s;
	EXPECT_EQ(db_session_get(sid, &s), -1);
	db_close();
	TEST_END();
}

static void test_settings_upsert(void)
{
	TEST_BEGIN("db_setting_set upserts; db_setting_get reads latest");
	ASSERT(db_open() == 0);

	EXPECT(db_setting_get("theme") == NULL);

	db_setting_set("theme", "dark");
	char *v = db_setting_get("theme");
	EXPECT_STREQ(v, "dark");
	free(v);

	db_setting_set("theme", "light");
	v = db_setting_get("theme");
	EXPECT_STREQ(v, "light");
	free(v);

	db_close();
	TEST_END();
}

int main(void)
{
	if (setup_tmp_home() != 0) {
		fprintf(stderr, "FATAL: could not set up temp HOME\n");
		return 2;
	}

	test_open_close_idempotent();
	test_session_roundtrip_preserves_messages();
	test_session_delete_cascades_to_messages();
	test_settings_upsert();

	teardown_tmp_home();
	return TEST_REPORT();
}
