#include "test.h"
#include "registry.h"

static void test_known_aliases_resolve(void)
{
	TEST_BEGIN("known aliases resolve to non-empty URLs");
	const char *aliases[] = {
		"llama-3.2-3b", "gemma-3-4b", "phi-4-mini",
		"qwen-coder-1.5b", "qwen-coder-7b", NULL
	};
	for (int i = 0; aliases[i]; i++) {
		const char *url = registry_lookup(aliases[i]);
		EXPECT(url != NULL);
		if (url) {
			EXPECT(strncmp(url, "https://", 8) == 0);
			EXPECT(strstr(url, ".gguf") != NULL);
		}
	}
	TEST_END();
}

static void test_unknown_alias_returns_null(void)
{
	TEST_BEGIN("unknown alias returns NULL");
	EXPECT(registry_lookup("does-not-exist") == NULL);
	EXPECT(registry_lookup("") == NULL);
	TEST_END();
}

static void test_registry_is_well_formed(void)
{
	TEST_BEGIN("every registry entry before the sentinel has all fields");
	for (int i = 0; registry[i].alias; i++) {
		EXPECT(registry[i].alias && registry[i].alias[0] != '\0');
		EXPECT(registry[i].url && registry[i].url[0] != '\0');
		EXPECT(registry[i].size && registry[i].size[0] != '\0');
		EXPECT(registry[i].quant && registry[i].quant[0] != '\0');
	}
	TEST_END();
}

static void test_lookup_is_case_sensitive(void)
{
	TEST_BEGIN("lookup is case-sensitive (aliases are lowercase)");
	EXPECT(registry_lookup("llama-3.2-3b") != NULL);
	EXPECT(registry_lookup("LLAMA-3.2-3B") == NULL);
	TEST_END();
}

int main(void)
{
	test_known_aliases_resolve();
	test_unknown_alias_returns_null();
	test_registry_is_well_formed();
	test_lookup_is_case_sensitive();
	return TEST_REPORT();
}
