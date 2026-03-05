#ifndef KORA_REGISTRY_H
#define KORA_REGISTRY_H

struct registry_entry {
	const char *alias;
	const char *url;
	const char *size;
	const char *quant;
};

/* null-terminated array of known models */
extern struct registry_entry registry[];

/* lookup a URL by alias, returns NULL if not found */
const char *registry_lookup(const char *alias);

#endif
