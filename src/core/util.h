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

/* redirect stderr to /dev/null, returns saved fd (or -1 on error) */
int kora_stderr_suppress(void);

/* restore stderr from saved fd */
void kora_stderr_restore(int saved_fd);

#endif
