#ifndef KORA_MODEL_H
#define KORA_MODEL_H

#include <stddef.h>   /* size_t */

/* list downloaded models in ~/.kora/models/ (stdout, for CLI) */
int model_list(void);

/* remove a model by name */
int model_rm(const char *name);

/* progress callback: called with percentage (0-100), downloaded MB, total MB */
typedef void (*model_progress_cb)(int pct, double dl_mb, double total_mb, void *user_data);

/* set a progress callback for model downloads (NULL = printf to stdout) */
void model_set_progress_cb(model_progress_cb cb, void *user_data);

/* install a pointer the downloader polls inside its progress callback — any
   non-zero value aborts the transfer. the .part file is unlinked on abort,
   same as any other curl failure. pass NULL to clear. */
void model_set_cancel_flag(volatile int *flag);

/* download a model by alias or direct URL */
int model_pull(const char *target);

/* check if a resolved model file exists */
int model_exists(const char *model_path);

/* read a GGUF file's metadata header and extract the human-readable
   `general.name` (optional: `general.size_label`) into caller buffers.
   returns 0 on success, -1 on any read/parse error. output buffers are
   NUL-terminated even on truncation. safe to pass NULL for fields you
   don't care about. reads only the first metadata block (a few KB at
   the head of the file); doesn't touch tensor data. */
int model_read_gguf_meta(const char *path,
                         char *name,       size_t name_sz,
                         char *size_label, size_t size_label_sz);

#endif
