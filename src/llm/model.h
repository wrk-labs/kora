#ifndef KORA_MODEL_H
#define KORA_MODEL_H

/* list downloaded models in ~/.kora/models/ (stdout, for CLI) */
int model_list(void);

/* remove a model by name */
int model_rm(const char *name);

/* progress callback: called with percentage (0-100), downloaded MB, total MB */
typedef void (*model_progress_cb)(int pct, double dl_mb, double total_mb, void *user_data);

/* set a progress callback for model downloads (NULL = printf to stdout) */
void model_set_progress_cb(model_progress_cb cb, void *user_data);

/* download a model by alias or direct URL */
int model_pull(const char *target);

/* check if a resolved model file exists */
int model_exists(const char *model_path);

#endif
