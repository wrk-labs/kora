#include "test.h"
#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char tmp_home[512];

static int setup_tmp_home(void)
{
	char tpl[] = "/tmp/kora-cfgtest-XXXXXX";
	char *p = mkdtemp(tpl);
	if (!p) return -1;
	snprintf(tmp_home, sizeof tmp_home, "%s", p);
	setenv("HOME", tmp_home, 1);
	char sub[600];
	snprintf(sub, sizeof sub, "%s/.kora", tmp_home);
	if (mkdir(sub, 0755) != 0 && errno != EEXIST) return -1;
	return 0;
}

static void teardown_tmp_home(void)
{
	char cmd[600];
	snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmp_home);
	(void)system(cmd);
}

static void write_user_config(const char *body)
{
	char path[700];
	snprintf(path, sizeof path, "%s/.kora/config.lua", tmp_home);
	FILE *f = fopen(path, "w");
	if (!f) return;
	fputs(body, f);
	fclose(f);
}

static void clear_user_config(void)
{
	char path[700];
	snprintf(path, sizeof path, "%s/.kora/config.lua", tmp_home);
	unlink(path);
}

static void test_defaults_when_no_user_config(void)
{
	TEST_BEGIN("defaults apply when no user config.lua exists");
	clear_user_config();
	struct kora_config *cfg = kora_config_load("lua");
	ASSERT(cfg != NULL);
	EXPECT_STREQ(cfg->default_model, "llama-3.2-3b");
	EXPECT_EQ(cfg->ctx_size, 4096);
	EXPECT(cfg->chat_model == NULL);
	kora_config_free(cfg);
	TEST_END();
}

static void test_user_config_overrides_defaults(void)
{
	TEST_BEGIN("user config.lua overrides defaults for declared fields");
	write_user_config(
		"return {\n"
		"  default_model = 'qwen-coder-7b',\n"
		"  chat_model    = 'llama-3.1-8b',\n"
		"  ctx_size      = 16384,\n"
		"}\n");
	struct kora_config *cfg = kora_config_load("lua");
	ASSERT(cfg != NULL);
	EXPECT_STREQ(cfg->default_model, "qwen-coder-7b");
	EXPECT_STREQ(cfg->chat_model, "llama-3.1-8b");
	EXPECT_EQ(cfg->ctx_size, 16384);
	kora_config_free(cfg);
	TEST_END();
}

static void test_partial_config_keeps_defaults_for_missing_fields(void)
{
	TEST_BEGIN("partial user config keeps defaults for omitted fields");
	write_user_config(
		"return {\n"
		"  ctx_size = 2048,\n"
		"}\n");
	struct kora_config *cfg = kora_config_load("lua");
	ASSERT(cfg != NULL);
	EXPECT_STREQ(cfg->default_model, "llama-3.2-3b");
	EXPECT_EQ(cfg->ctx_size, 2048);
	kora_config_free(cfg);
	TEST_END();
}

static void test_empty_config_table_is_safe(void)
{
	TEST_BEGIN("empty config table yields all defaults");
	write_user_config("return {}\n");
	struct kora_config *cfg = kora_config_load("lua");
	ASSERT(cfg != NULL);
	EXPECT_STREQ(cfg->default_model, "llama-3.2-3b");
	EXPECT_EQ(cfg->ctx_size, 4096);
	kora_config_free(cfg);
	TEST_END();
}

static void test_malformed_config_falls_back_to_defaults(void)
{
	TEST_BEGIN("syntax error in config.lua leaves defaults intact");
	write_user_config("this is not valid lua !!!!\n");
	struct kora_config *cfg = kora_config_load("lua");
	ASSERT(cfg != NULL);
	EXPECT_STREQ(cfg->default_model, "llama-3.2-3b");
	EXPECT_EQ(cfg->ctx_size, 4096);
	kora_config_free(cfg);
	TEST_END();
}

int main(void)
{
	if (setup_tmp_home() != 0) {
		fprintf(stderr, "FATAL: could not set up temp HOME\n");
		return 2;
	}

	test_defaults_when_no_user_config();
	test_user_config_overrides_defaults();
	test_partial_config_keeps_defaults_for_missing_fields();
	test_empty_config_table_is_safe();
	test_malformed_config_falls_back_to_defaults();

	teardown_tmp_home();
	return TEST_REPORT();
}
