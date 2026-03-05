#include <string.h>

#include "registry.h"

struct registry_entry registry[] = {
	{ "llama-3.2-3b",   "https://huggingface.co/bartowski/Llama-3.2-3B-Instruct-GGUF/resolve/main/Llama-3.2-3B-Instruct-Q4_K_M.gguf",          "2.0G", "Q4_K_M" },
	{ "qwen-coder-7b",  "https://huggingface.co/bartowski/Qwen2.5-Coder-7B-Instruct-GGUF/resolve/main/Qwen2.5-Coder-7B-Instruct-Q4_K_M.gguf",  "4.5G", "Q4_K_M" },
	{ "deepseek-r1-8b", "https://huggingface.co/bartowski/DeepSeek-R1-Distill-Llama-8B-GGUF/resolve/main/DeepSeek-R1-Distill-Llama-8B-Q4_K_M.gguf", "4.9G", "Q4_K_M" },
	{ NULL, NULL, NULL, NULL },
};

const char *registry_lookup(const char *alias)
{
	int i;
	for (i = 0; registry[i].alias; i++) {
		if (strcmp(registry[i].alias, alias) == 0)
			return registry[i].url;
	}
	return NULL;
}
