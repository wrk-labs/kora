#ifndef KORA_UTIL_H
#define KORA_UTIL_H

/* ensure ~/.kora/ and subdirectories exist */
int kora_init_dirs(void);

/* return path to ~/.kora/<sub>, caller must free */
char *kora_path(const char *sub);

/* read ~/.kora/preferred_model, returns allocated string or NULL */
char *kora_preferred_model(void);

/* write model name to ~/.kora/preferred_model */
void kora_set_preferred_model(const char *name);

#endif
