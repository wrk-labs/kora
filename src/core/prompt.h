#ifndef KORA_PROMPT_H
#define KORA_PROMPT_H

/* Render a system-prompt template by substituting a fixed set of
   environment placeholders:

     {date}      today's date, e.g. "Friday, April 24, 2026"
     {time}      local time + UTC offset, e.g. "09:40 -03"
     {platform}  uname -s lowercased, e.g. "darwin" / "linux"
     {model}     the passed model alias, or "(unknown)" if NULL/empty
     {ctx}       the passed ctx_size as decimal, or "(unknown)" if <= 0

   Unknown placeholders are left verbatim in the output so that typos in
   a user-edited template are visible at runtime rather than silently
   eaten. A NULL template yields NULL.

   Returns a newly-allocated C string the caller must free(). */
char *kora_prompt_render(const char *tmpl, const char *model, int ctx_size);

#endif
