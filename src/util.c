#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "util.h"

static int mkdirp(const char *path)
{
	struct stat st;
	if (stat(path, &st) == 0)
		return 0;
	return mkdir(path, 0755);
}

char *kora_path(const char *sub)
{
	const char *home = getenv("HOME");
	if (!home)
		return NULL;

	size_t len = strlen(home) + strlen("/.kora/") + (sub ? strlen(sub) : 0) + 1;
	char *path = malloc(len);
	if (!path)
		return NULL;

	if (sub)
		snprintf(path, len, "%s/.kora/%s", home, sub);
	else
		snprintf(path, len, "%s/.kora", home);

	return path;
}

int kora_init_dirs(void)
{
	char *dirs[] = { "", "models", "sessions", "plugins" };
	int i;

	for (i = 0; i < 4; i++) {
		char *path = kora_path(dirs[i]);
		if (!path)
			return -1;
		if (mkdirp(path) != 0) {
			free(path);
			return -1;
		}
		free(path);
	}
	return 0;
}
