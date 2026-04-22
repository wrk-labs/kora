#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <curl/curl.h>

#include "model.h"
#include "registry.h"
#include "util.h"

/* ------------------------------------------------------------------
   GGUF metadata reader.

   File layout (little-endian throughout):
     magic:   char[4]    = "GGUF"
     version: u32
     tensor_count: u64
     metadata_kv_count: u64
     kv[metadata_kv_count]:
       key:        string (u64 len + bytes, no NUL)
       value_type: u32
       value:      depends on value_type
   ------------------------------------------------------------------ */

enum {
	GGUF_T_U8 = 0, GGUF_T_I8 = 1, GGUF_T_U16 = 2, GGUF_T_I16 = 3,
	GGUF_T_U32 = 4, GGUF_T_I32 = 5, GGUF_T_F32 = 6, GGUF_T_BOOL = 7,
	GGUF_T_STRING = 8, GGUF_T_ARRAY = 9,
	GGUF_T_U64 = 10, GGUF_T_I64 = 11, GGUF_T_F64 = 12,
};

static int gguf_read_u32(FILE *f, uint32_t *out)
{
	unsigned char b[4];
	if (fread(b, 1, 4, f) != 4) return -1;
	*out = (uint32_t)b[0]       | ((uint32_t)b[1] << 8) |
	       ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
	return 0;
}

static int gguf_read_u64(FILE *f, uint64_t *out)
{
	unsigned char b[8];
	if (fread(b, 1, 8, f) != 8) return -1;
	uint64_t v = 0;
	for (int i = 0; i < 8; i++) v |= ((uint64_t)b[i]) << (i * 8);
	*out = v;
	return 0;
}

static size_t gguf_fixed_size(uint32_t type)
{
	switch (type) {
	case GGUF_T_U8: case GGUF_T_I8: case GGUF_T_BOOL: return 1;
	case GGUF_T_U16: case GGUF_T_I16:                 return 2;
	case GGUF_T_U32: case GGUF_T_I32: case GGUF_T_F32: return 4;
	case GGUF_T_U64: case GGUF_T_I64: case GGUF_T_F64: return 8;
	default: return 0;
	}
}

static int gguf_read_string(FILE *f, char *dst, size_t dst_sz)
{
	uint64_t len;
	if (gguf_read_u64(f, &len) < 0) return -1;
	if (len > (1UL << 20)) return -1;   /* >1MB key/value is junk */
	size_t copy = (len < dst_sz - 1) ? (size_t)len : dst_sz - 1;
	if (copy > 0 && fread(dst, 1, copy, f) != copy) return -1;
	dst[copy] = '\0';
	if (copy < len) {
		if (fseek(f, (long)(len - copy), SEEK_CUR) != 0) return -1;
	}
	return 0;
}

static int gguf_skip_string(FILE *f)
{
	uint64_t len;
	if (gguf_read_u64(f, &len) < 0) return -1;
	if (len > (1UL << 20)) return -1;
	if (fseek(f, (long)len, SEEK_CUR) != 0) return -1;
	return 0;
}

static int gguf_skip_value(FILE *f, uint32_t type)
{
	if (type == GGUF_T_STRING) return gguf_skip_string(f);
	if (type == GGUF_T_ARRAY) {
		uint32_t et;
		uint64_t n;
		if (gguf_read_u32(f, &et) < 0) return -1;
		if (gguf_read_u64(f, &n) < 0) return -1;
		if (n > (1UL << 24)) return -1;  /* sanity */
		for (uint64_t i = 0; i < n; i++)
			if (gguf_skip_value(f, et) < 0) return -1;
		return 0;
	}
	size_t sz = gguf_fixed_size(type);
	if (sz == 0) return -1;   /* unknown type: bail */
	return fseek(f, (long)sz, SEEK_CUR);
}

int model_read_gguf_meta(const char *path,
                         char *name, size_t name_sz,
                         char *size_label, size_t size_label_sz)
{
	if (name && name_sz) name[0] = '\0';
	if (size_label && size_label_sz) size_label[0] = '\0';

	FILE *f = fopen(path, "rb");
	if (!f) return -1;

	char magic[4];
	if (fread(magic, 1, 4, f) != 4 ||
	    magic[0] != 'G' || magic[1] != 'G' ||
	    magic[2] != 'U' || magic[3] != 'F') {
		fclose(f); return -1;
	}

	uint32_t version;
	uint64_t tensor_count, kv_count;
	if (gguf_read_u32(f, &version) < 0 ||
	    gguf_read_u64(f, &tensor_count) < 0 ||
	    gguf_read_u64(f, &kv_count) < 0) {
		fclose(f); return -1;
	}
	if (kv_count > 4096) { fclose(f); return -1; }   /* sanity cap */

	for (uint64_t i = 0; i < kv_count; i++) {
		char key[128];
		if (gguf_read_string(f, key, sizeof key) < 0) { fclose(f); return -1; }
		uint32_t vtype;
		if (gguf_read_u32(f, &vtype) < 0) { fclose(f); return -1; }

		int want_name = (vtype == GGUF_T_STRING && name && name_sz > 0 &&
		                 !name[0] && strcmp(key, "general.name") == 0);
		int want_size = (vtype == GGUF_T_STRING && size_label &&
		                 size_label_sz > 0 && !size_label[0] &&
		                 strcmp(key, "general.size_label") == 0);

		if (want_name) {
			if (gguf_read_string(f, name, name_sz) < 0) {
				fclose(f); return -1;
			}
		} else if (want_size) {
			if (gguf_read_string(f, size_label, size_label_sz) < 0) {
				fclose(f); return -1;
			}
		} else {
			if (gguf_skip_value(f, vtype) < 0) { fclose(f); return -1; }
		}
	}

	fclose(f);
	return 0;
}

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

/* GGUF files begin with the 4-byte magic "GGUF" (0x47 47 55 46). any other
   prefix (HTML error page, JSON, empty file) means the URL pointed at
   something that isn't a model. returns 1 on match, 0 otherwise. */
static int is_gguf_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) return 0;
	char magic[4] = {0};
	size_t got = fread(magic, 1, sizeof magic, f);
	fclose(f);
	return got == sizeof magic &&
	       magic[0] == 'G' && magic[1] == 'G' &&
	       magic[2] == 'U' && magic[3] == 'F';
}

static model_progress_cb ext_progress_cb = NULL;
static void *ext_progress_data = NULL;
static volatile int *ext_cancel_flag = NULL;

void model_set_progress_cb(model_progress_cb cb, void *user_data)
{
	ext_progress_cb = cb;
	ext_progress_data = user_data;
}

void model_set_cancel_flag(volatile int *flag)
{
	ext_cancel_flag = flag;
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

	if (ext_cancel_flag && *ext_cancel_flag)
		return 1;  /* aborts curl with CURLE_ABORTED_BY_CALLBACK */

	if (dltotal <= 0)
		return 0;

	int pct = (int)(dlnow * 100 / dltotal);
	if (pct != pd->last_pct) {
		pd->last_pct = pct;
		double dl_mb = (double)dlnow / (1024.0 * 1024.0);
		double total_mb = (double)dltotal / (1024.0 * 1024.0);

		if (ext_progress_cb) {
			ext_progress_cb(pct, dl_mb, total_mb, ext_progress_data);
		} else {
			printf("\r  %s: %.0fMB / %.0fMB (%d%%)", pd->filename, dl_mb, total_mb, pct);
			fflush(stdout);
		}
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
		printf("No models downloaded.\n");
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
		printf("No models downloaded.\n");

	return 0;
}

int model_rm(const char *name)
{
	/* try alias first */
	const char *url = registry_lookup(name);
	char fname_buf[256];
	const char *filename;
	if (url) {
		filename = filename_from_url(url);
	} else {
		/* manual / non-registry alias: db_model_add stores the alias
		   with ".gguf" stripped, so reconstruct the filename here.
		   mirrors resolve_model_path() in main.c and pool.c. */
		size_t nlen = strlen(name);
		int has_ext = (nlen >= 5 && strcmp(name + nlen - 5, ".gguf") == 0);
		if (has_ext) {
			filename = name;
		} else {
			snprintf(fname_buf, sizeof fname_buf, "%s.gguf", name);
			filename = fname_buf;
		}
	}

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

int model_exists(const char *model_path)
{
	return access(model_path, F_OK) == 0;
}

int model_pull(const char *target)
{
	const char *url;
	const char *alias = NULL;

	int tui_mode = (ext_progress_cb != NULL);

	/* if it looks like a URL, use directly */
	if (strncmp(target, "http://", 7) == 0 || strncmp(target, "https://", 8) == 0) {
		url = target;
	} else {
		url = registry_lookup(target);
		if (!url) {
			if (!tui_mode) {
				fprintf(stderr, "kora: unknown model '%s'\n\navailable models:\n", target);
				int i;
				for (i = 0; registry[i].alias; i++)
					fprintf(stderr, "  %-20s %s  %s\n", registry[i].alias, registry[i].size, registry[i].quant);
				fprintf(stderr, "\nor provide a direct URL: kora pull https://...\n");
			}
			return -1;
		}
		alias = target;
	}

	/* strip any URL query string from the filename — URLs like
	   .../foo.gguf?download=true would otherwise land on disk as
	   "foo.gguf?download=true" which then confuses every downstream
	   tool (display, rm, re-pull, etc.). */
	const char *filename_raw = filename_from_url(url);
	char filename[256];
	size_t flen = strlen(filename_raw);
	const char *q = strchr(filename_raw, '?');
	if (q) flen = (size_t)(q - filename_raw);
	if (flen >= sizeof filename) flen = sizeof filename - 1;
	memcpy(filename, filename_raw, flen);
	filename[flen] = '\0';

	char sub[512];
	snprintf(sub, sizeof(sub), "models/%s", filename);

	char *dest = kora_path(sub);
	if (!dest)
		return -1;

	/* check if already downloaded — but only accept a real GGUF. a prior
	   failed pull (wrong URL, saved HTML into a .gguf name) would otherwise
	   be treated as a successful install and reported as "complete". if
	   the existing file isn't GGUF, unlink it and fall through to re-pull. */
	if (access(dest, F_OK) == 0) {
		if (is_gguf_file(dest)) {
			if (!tui_mode)
				printf("model '%s' already downloaded\n",
				       alias ? alias : filename);
			free(dest);
			return 0;
		}
		if (!tui_mode)
			fprintf(stderr,
				"kora: existing file is not GGUF — removing and re-pulling\n");
		unlink(dest);
	}

	/* download to temp file first */
	char tmp[512];
	snprintf(tmp, sizeof(tmp), "%s.part", dest);

	if (!tui_mode)
		printf("pulling %s\n", alias ? alias : filename);

	CURL *curl = curl_easy_init();
	if (!curl) {
		if (!tui_mode)
			fprintf(stderr, "kora: failed to initialize curl\n");
		free(dest);
		return -1;
	}

	FILE *fp = fopen(tmp, "wb");
	if (!fp) {
		if (!tui_mode)
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
		if (!tui_mode)
			fprintf(stderr, "\nkora: download failed: %s\n", curl_easy_strerror(res));
		unlink(tmp);
		free(dest);
		return -1;
	}

	/* HF model pages (and other wrong URLs) return 200 with HTML, which
	   curl happily saves. reject anything that isn't a GGUF. */
	if (!is_gguf_file(tmp)) {
		if (!tui_mode)
			fprintf(stderr, "\nkora: not a GGUF file (wrong URL?)\n");
		unlink(tmp);
		free(dest);
		return -1;
	}

	/* move temp file to final destination */
	if (rename(tmp, dest) != 0) {
		if (!tui_mode)
			fprintf(stderr, "\nkora: failed to save model\n");
		unlink(tmp);
		free(dest);
		return -1;
	}

	if (!tui_mode)
		printf("\ndone\n");
	free(dest);
	return 0;
}
