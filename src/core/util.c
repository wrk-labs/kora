#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

/* 0 on success, -1 on any open/read/write failure. */
static int copy_file(const char *src, const char *dst)
{
	FILE *in = fopen(src, "r");
	if (!in)
		return -1;
	FILE *out = fopen(dst, "w");
	if (!out) {
		fclose(in);
		return -1;
	}
	char buf[4096];
	size_t n;
	int rc = 0;
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
		if (fwrite(buf, 1, n, out) != n) { rc = -1; break; }
	}
	if (ferror(in)) rc = -1;
	fclose(in);
	if (fclose(out) != 0) rc = -1;
	return rc;
}

int kora_init_dirs(void)
{
	const char *dirs[] = { "", "models", "sessions" };
	size_t i;

	for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		char *path = kora_path(dirs[i]);
		if (!path)
			return -1;
		if (mkdirp(path) != 0) {
			free(path);
			return -1;
		}
		free(path);
	}

	/* copy default config.lua if not present */
	char *cfg_dst = kora_path("config.lua");
	if (cfg_dst) {
		if (access(cfg_dst, F_OK) != 0) {
			char src[512];
			snprintf(src, sizeof(src), "%s/core/config.lua", LUADIR);
			if (access(src, F_OK) != 0)
				snprintf(src, sizeof(src), "lua/core/config.lua");
			copy_file(src, cfg_dst);
		}
		free(cfg_dst);
	}

	return 0;
}

char *kora_preferred_model(void)
{
	char *path = kora_path("preferred_model");
	if (!path)
		return NULL;

	FILE *f = fopen(path, "r");
	free(path);
	if (!f)
		return NULL;

	char buf[256];
	if (!fgets(buf, sizeof(buf), f)) {
		fclose(f);
		return NULL;
	}
	fclose(f);

	buf[strcspn(buf, "\n")] = 0;
	if (buf[0] == '\0')
		return NULL;

	return strdup(buf);
}

void kora_set_preferred_model(const char *name)
{
	char *path = kora_path("preferred_model");
	if (!path)
		return;

	FILE *f = fopen(path, "w");
	free(path);
	if (!f)
		return;

	fprintf(f, "%s\n", name);
	fclose(f);
}

int kora_stderr_suppress(void)
{
	int saved = dup(STDERR_FILENO);
	if (saved < 0)
		return -1;

	int devnull = open("/dev/null", O_WRONLY);
	if (devnull < 0) {
		close(saved);
		return -1;
	}

	dup2(devnull, STDERR_FILENO);
	close(devnull);
	return saved;
}

void kora_stderr_restore(int saved_fd)
{
	if (saved_fd < 0)
		return;
	dup2(saved_fd, STDERR_FILENO);
	close(saved_fd);
}
