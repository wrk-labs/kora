#ifndef KORA_UTIL_H
#define KORA_UTIL_H

/* ensure ~/.kora/ and subdirectories exist */
int kora_init_dirs(void);

/* return path to ~/.kora/<sub>, caller must free */
char *kora_path(const char *sub);

#endif
