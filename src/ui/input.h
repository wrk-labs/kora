#ifndef KORA_INPUT_H
#define KORA_INPUT_H

/* initialize terminal for raw input */
void input_init(void);

/* restore terminal to original state */
void input_cleanup(void);

/* read a line with prompt, with arrow key history and basic line editing
   returns allocated string (caller must free), or NULL on EOF */
char *input_read(const char *prompt);

#endif
