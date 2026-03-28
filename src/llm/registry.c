#include <string.h>

#include "registry.h"

struct registry_entry registry[] = {
	/* chat models */
	{ "llama-3.2-3b",    "https://huggingface.co/bartowski/Llama-3.2-3B-Instruct-GGUF/resolve/main/Llama-3.2-3B-Instruct-Q4_K_M.gguf",                    "2.0G", "Q4_K_M" },
	{ "gemma-3-4b",      "https://huggingface.co/bartowski/google_gemma-3-4b-it-GGUF/resolve/main/google_gemma-3-4b-it-Q4_K_M.gguf",                       "2.8G", "Q4_K_M" },
	{ "phi-4-mini",      "https://huggingface.co/bartowski/microsoft_Phi-4-mini-instruct-GGUF/resolve/main/microsoft_Phi-4-mini-instruct-Q4_K_M.gguf",     "2.3G", "Q4_K_M" },
	{ "qwen-3-8b",       "https://huggingface.co/bartowski/Qwen_Qwen3-8B-GGUF/resolve/main/Qwen_Qwen3-8B-Q4_K_M.gguf",                                   "5.0G", "Q4_K_M" },
	{ "llama-3.1-8b",    "https://huggingface.co/bartowski/Meta-Llama-3.1-8B-Instruct-GGUF/resolve/main/Meta-Llama-3.1-8B-Instruct-Q4_K_M.gguf",          "4.9G", "Q4_K_M" },
	{ "deepseek-r1-8b",  "https://huggingface.co/bartowski/DeepSeek-R1-Distill-Llama-8B-GGUF/resolve/main/DeepSeek-R1-Distill-Llama-8B-Q4_K_M.gguf",      "4.9G", "Q4_K_M" },
	/* coding models */
	{ "qwen-coder-1.5b", "https://huggingface.co/bartowski/Qwen2.5-Coder-1.5B-Instruct-GGUF/resolve/main/Qwen2.5-Coder-1.5B-Instruct-Q4_K_M.gguf",       "1.0G", "Q4_K_M" },
	{ "qwen-coder-7b",   "https://huggingface.co/bartowski/Qwen2.5-Coder-7B-Instruct-GGUF/resolve/main/Qwen2.5-Coder-7B-Instruct-Q4_K_M.gguf",           "4.5G", "Q4_K_M" },
	{ "qwen-coder-14b",  "https://huggingface.co/bartowski/Qwen2.5-Coder-14B-Instruct-GGUF/resolve/main/Qwen2.5-Coder-14B-Instruct-Q4_K_M.gguf",         "8.7G", "Q4_K_M" },
	{ "devstral-small",  "https://huggingface.co/bartowski/mistralai_Devstral-Small-2505-GGUF/resolve/main/mistralai_Devstral-Small-2505-Q4_K_M.gguf",     "14G",  "Q4_K_M" },
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
