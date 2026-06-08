/*
 * util.h — small filesystem / string helpers shared by db.c + install.c.
 *
 * Kept deliberately tiny. Anything more elaborate (an arena, a
 * vec<T>, etc.) lives in its own header.
 */

#ifndef FTR_UTIL_H
#define FTR_UTIL_H

#include <stddef.h>
#include <sys/types.h>

/* Concatenate `n` strings with '/' separators into a freshly-malloc'd
 * buffer. Empty components are skipped; consecutive slashes are
 * collapsed. Returns NULL on OOM. */
char *path_join(int n, ...);

/* `mkdir -p` semantics. Returns 0 on success or if the directory
 * already existed. Returns -1 on error and sets errno. */
int mkdir_p(const char *path, mode_t mode);

/* Recursively remove a directory tree (`rm -rf`). Returns 0 on
 * success, -1 on error. */
int rm_rf(const char *path);

/* Copy one regular file from src to dst, preserving the source's
 * mode. Truncates dst if it exists. Returns 0 / -1. */
int copy_file(const char *src, const char *dst);

/* Recursively copy a directory tree. Creates `dst` if needed.
 * Handles regular files, directories, and symlinks. Returns 0 / -1. */
int copy_tree(const char *src, const char *dst);

/* String compare for qsort. */
int strcmp_qsort(const void *a, const void *b);

/* Does `path` exist? Returns 1 yes, 0 no. */
int path_exists(const char *path);

/* Is `path` a directory? Returns 1 yes, 0 no (incl. ENOENT). */
int is_dir(const char *path);

/* Compare two dotted version strings (e.g. "0.10.0" vs "0.2.0").
 * Splits on '.' and compares each numeric segment; non-numeric
 * components fall back to a strcmp on the segment. Returns
 *   < 0 if a < b
 *   = 0 if a == b
 *   > 0 if a > b
 */
int version_compare(const char *a, const char *b);

#endif /* FTR_UTIL_H */
