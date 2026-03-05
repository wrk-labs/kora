#ifndef KORA_MODEL_H
#define KORA_MODEL_H

/* list downloaded models in ~/.kora/models/ */
int model_list(void);

/* remove a model by name */
int model_rm(const char *name);

/* download a model by alias or direct URL */
int model_pull(const char *target);

/* check if a resolved model file exists */
int model_exists(const char *model_path);

#endif
