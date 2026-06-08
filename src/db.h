/*
 * db.h — local install database at /var/lib/feather/local/.
 *
 * Format mirrors pacman's "directory of text files" approach. For
 * each installed package we materialise:
 *
 *   <DB_ROOT>/local/<name>-<version>/
 *   ├── manifest.toml      copy of the package's manifest
 *   ├── files              newline-separated, sorted, paths relative to /
 *   ├── installed.txt      ISO 8601 timestamp of the install
 *   └── hooks/             copy of the .feather archive's hooks/ tree
 *
 * $FTR_DB_ROOT env var overrides the default DB root (FTR_DB_ROOT
 * compile-time constant from common.h, i.e. /var/lib/feather). Tests
 * point it at a sandbox dir.
 */

#ifndef FTR_DB_H
#define FTR_DB_H

#include <stddef.h>

/* One DB entry as returned by ftr_db_get(). Strings + arrays are
 * heap-allocated; release with ftr_db_entry_free(). */
typedef struct {
	char *name;
	char *version;
	char *installed_at;    /* ISO 8601 string; NULL if not recorded */
	char **files;          /* paths relative to /, sorted */
	size_t n_files;
} ftr_db_entry;

/* Effective DB root: honors $FTR_DB_ROOT, falls back to the
 * FTR_DB_ROOT compile-time default. The returned pointer is
 * statically owned and stays valid until the next call. NOT
 * thread-safe — fine because ftr is single-threaded. */
const char *ftr_db_root(void);

/* Record a freshly-installed package.
 *
 *   manifest_src_path : path to the package's manifest.toml inside
 *                       the unpacked .feather tempdir; copied verbatim
 *                       into the DB.
 *   hooks_src_dir     : optional path to the unpacked hooks/ dir.
 *                       NULL or non-existent => skip.
 *   files / n_files   : paths relative to / (e.g. "peacock/bin/foo").
 *
 * Returns 0 on success, -1 on failure (diagnostic on stderr). */
int ftr_db_record(const char *name, const char *version,
                  const char *manifest_src_path,
                  const char *hooks_src_dir,
                  char **files, size_t n_files);

/* List every installed package as a newly-allocated array of
 * "name-version" strings. Caller frees each string + the array.
 * On empty DB sets *names_out = NULL, *n_out = 0, returns 0. */
int ftr_db_list(char ***names_out, size_t *n_out);

/* Resolve a name (no version) to its latest installed version, or
 * NULL if not installed. Caller frees the returned string. */
char *ftr_db_find_version(const char *name);

/* Load one DB entry by (name, version). Returns 0 on success, -1 if
 * the entry is missing. On success *out is populated and must be
 * released with ftr_db_entry_free(). */
int ftr_db_get(const char *name, const char *version, ftr_db_entry *out);

/* Free heap fields on a populated entry. Safe on zero-initialised. */
void ftr_db_entry_free(ftr_db_entry *e);

/* Path to <DB_ROOT>/local/<name>-<version>/hooks/<hook_name>.sh, or
 * NULL if no such hook is recorded. Caller frees. */
char *ftr_db_hook_path(const char *name, const char *version,
                       const char *hook_name);

/* Remove the DB entry for (name, version). Does NOT touch the
 * actual installed files on disk — caller does that separately. */
int ftr_db_remove(const char *name, const char *version);

#endif /* FTR_DB_H */
