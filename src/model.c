#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <curl/curl.h>

#include "model.h"
#include "registry.h"
#include "util.h"

static const char *filename_from_url(const char *url)
{
	const char *slash = strrchr(url, '/');
	return slash ? slash + 1 : url;
}

static int has_suffix(const char *str, const char *suffix)
{
	size_t slen = strlen(str);
	size_t xlen = strlen(suffix);
	if (xlen > slen)
		return 0;
	return strcmp(str + slen - xlen, suffix) == 0;
}

struct progress_data {
	const char *filename;
	int last_pct;
};

static int progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                        curl_off_t ultotal, curl_off_t ulnow)
{
	struct progress_data *pd = clientp;
	(void)ultotal;
	(void)ulnow;

	if (dltotal <= 0)
		return 0;

	int pct = (int)(dlnow * 100 / dltotal);
	if (pct != pd->last_pct) {
		pd->last_pct = pct;
		double dl_mb = (double)dlnow / (1024.0 * 1024.0);
		double total_mb = (double)dltotal / (1024.0 * 1024.0);
		printf("\r  %s: %.0fMB / %.0fMB (%d%%)", pd->filename, dl_mb, total_mb, pct);
		fflush(stdout);
	}
	return 0;
}

int model_list(void)
{
	char *dir = kora_path("models");
	if (!dir)
		return -1;

	DIR *d = opendir(dir);
	if (!d) {
		free(dir);
		printf("no models downloaded\n");
		return 0;
	}

	struct dirent *ent;
	int count = 0;

	while ((ent = readdir(d)) != NULL) {
		if (!has_suffix(ent->d_name, ".gguf"))
			continue;

		/* check alias */
		const char *alias = NULL;
		int i;
		for (i = 0; registry[i].alias; i++) {
			const char *fname = filename_from_url(registry[i].url);
			if (strcmp(ent->d_name, fname) == 0) {
				alias = registry[i].alias;
				break;
			}
		}

		if (alias)
			printf("  %s (%s)\n", alias, ent->d_name);
		else
			printf("  %s\n", ent->d_name);
		count++;
	}

	closedir(d);
	free(dir);

	if (count == 0)
		printf("no models downloaded\n");

	return 0;
}

int model_rm(const char *name)
{
	/* try alias first */
	const char *url = registry_lookup(name);
	const char *filename;
	if (url)
		filename = filename_from_url(url);
	else
		filename = name;

	char sub[512];
	snprintf(sub, sizeof(sub), "models/%s", filename);

	char *path = kora_path(sub);
	if (!path)
		return -1;

	if (access(path, F_OK) != 0) {
		fprintf(stderr, "kora: model '%s' not found\n", name);
		free(path);
		return -1;
	}

	if (unlink(path) != 0) {
		fprintf(stderr, "kora: failed to remove '%s'\n", path);
		free(path);
		return -1;
	}

	printf("removed %s\n", filename);
	free(path);
	return 0;
}

int model_pull(const char *target)
{
	const char *url;
	const char *alias = NULL;

	/* if it looks like a URL, use directly */
	if (strncmp(target, "http://", 7) == 0 || strncmp(target, "https://", 8) == 0) {
		url = target;
	} else {
		url = registry_lookup(target);
		if (!url) {
			fprintf(stderr, "kora: unknown model '%s'\n\navailable models:\n", target);
			int i;
			for (i = 0; registry[i].alias; i++)
				fprintf(stderr, "  %-20s %s  %s\n", registry[i].alias, registry[i].size, registry[i].quant);
			fprintf(stderr, "\nor provide a direct URL: kora pull https://...\n");
			return -1;
		}
		alias = target;
	}

	const char *filename = filename_from_url(url);
	char sub[512];
	snprintf(sub, sizeof(sub), "models/%s", filename);

	char *dest = kora_path(sub);
	if (!dest)
		return -1;

	/* check if already downloaded */
	if (access(dest, F_OK) == 0) {
		printf("model '%s' already downloaded\n", alias ? alias : filename);
		free(dest);
		return 0;
	}

	/* download to temp file first */
	char tmp[512];
	snprintf(tmp, sizeof(tmp), "%s.part", dest);

	printf("pulling %s\n", alias ? alias : filename);

	CURL *curl = curl_easy_init();
	if (!curl) {
		fprintf(stderr, "kora: failed to initialize curl\n");
		free(dest);
		return -1;
	}

	FILE *fp = fopen(tmp, "wb");
	if (!fp) {
		fprintf(stderr, "kora: failed to open '%s' for writing\n", tmp);
		curl_easy_cleanup(curl);
		free(dest);
		return -1;
	}

	struct progress_data pd = { .filename = alias ? alias : filename, .last_pct = -1 };

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &pd);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

	CURLcode res = curl_easy_perform(curl);
	fclose(fp);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK) {
		fprintf(stderr, "\nkora: download failed: %s\n", curl_easy_strerror(res));
		unlink(tmp);
		free(dest);
		return -1;
	}

	/* move temp file to final destination */
	if (rename(tmp, dest) != 0) {
		fprintf(stderr, "\nkora: failed to save model\n");
		unlink(tmp);
		free(dest);
		return -1;
	}

	printf("\ndone\n");
	free(dest);
	return 0;
}
