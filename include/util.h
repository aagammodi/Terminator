#ifndef TERMINATOR_UTIL_H
#define TERMINATOR_UTIL_H

#include "common.h"

void *xmalloc(size_t size);
void *xcalloc(size_t nmemb, size_t size);
void *xrealloc(void *ptr, size_t size);
char *xstrdup(const char *s);

/* Replace trailing newlines and whitespace */
void str_trim_trailing_newlines(char *s);

/* Path helpers */
void path_expand_tilde(const char *path, char *out, size_t out_size);
void path_compact_home(const char *path, char *out, size_t out_size);

/* Log / debug helper */
void term_log(const char *fmt, ...);

#endif /* TERMINATOR_UTIL_H */
