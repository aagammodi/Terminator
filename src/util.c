#include "util.h"
#include <stdarg.h>

void *xmalloc(size_t size) {
    if (size == 0) return NULL;
    void *ptr = malloc(size);
    if (!ptr) {
        fprintf(stderr, "terminator: out of memory (malloc %zu bytes)\n", size);
        exit(EXIT_FAILURE);
    }
    return ptr;
}

void *xcalloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0) return NULL;
    void *ptr = calloc(nmemb, size);
    if (!ptr) {
        fprintf(stderr, "terminator: out of memory (calloc %zu * %zu bytes)\n", nmemb, size);
        exit(EXIT_FAILURE);
    }
    return ptr;
}

void *xrealloc(void *ptr, size_t size) {
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    void *new_ptr = realloc(ptr, size);
    if (!new_ptr) {
        fprintf(stderr, "terminator: out of memory (realloc %zu bytes)\n", size);
        exit(EXIT_FAILURE);
    }
    return new_ptr;
}

char *xstrdup(const char *s) {
    if (!s) return NULL;
    char *copy = strdup(s);
    if (!copy) {
        fprintf(stderr, "terminator: out of memory (strdup)\n");
        exit(EXIT_FAILURE);
    }
    return copy;
}

void str_trim_trailing_newlines(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[len - 1] = '\0';
        len--;
    }
}

void path_expand_tilde(const char *path, char *out, size_t out_size) {
    if (!path || !out || out_size == 0) return;
    
    if (path[0] == '~') {
        const char *home = getenv("HOME");
        if (!home) {
            struct passwd *pw = getpwuid(getuid());
            if (pw) home = pw->pw_dir;
        }
        
        if (home) {
            if (path[1] == '/' || path[1] == '\0') {
                snprintf(out, out_size, "%s%s", home, path + 1);
                return;
            }
        }
    }
    safe_strncpy(out, path, out_size);
}

void path_compact_home(const char *path, char *out, size_t out_size) {
    if (!path || !out || out_size == 0) return;
    
    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    
    if (home) {
        size_t home_len = strlen(home);
        if (strncmp(path, home, home_len) == 0 && (path[home_len] == '/' || path[home_len] == '\0')) {
            snprintf(out, out_size, "~%s", path + home_len);
            return;
        }
    }
    safe_strncpy(out, path, out_size);
}

void term_log(const char *fmt, ...) {
    if (!getenv("TERMINATOR_DEBUG")) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}
