/*
 * repo.h — repository config + index reader + file:// fetch.
 *
 * Phase 4b (post-baseline phase 4): feather grows a tiny repo client
 * sufficient to serve packages out of a local directory. URL schemes
 * other than `file://` are stubbed with a phase-6 message — they don't
 * fail the call, they just skip the entry.
 *
 * Repo layout on disk:
 *
 *   <repo-root>/
 *   ├── index.toml                  metadata + package list
 *   └── <name>-<version>.feather    one or more package archives
 *
 * /etc/feather/feather.conf format:
 *
 *   [[repos]]
 *   name = "peacock-stable"
 *   url  = "file:///srv/peacock/repo"
 *
 * After `ftr sync`, the index lands at:
 *
 *   $FTR_DB_ROOT/sync/<repo-name>/index.toml
 */

#ifndef FTR_REPO_H
#define FTR_REPO_H

#include <stddef.h>

/* One [[repos]] entry from feather.conf. Strings are heap-owned. */
typedef struct {
	char *name;
	char *url;
} ftr_repo_cfg;

typedef struct {
	ftr_repo_cfg *items;
	size_t n;
} ftr_repo_list;

/* One [[package]] entry from a repo's index.toml. Strings are heap-owned. */
typedef struct {
	char *name;
	char *version;
	char *description; /* may be NULL */
	char *runtime;     /* may be NULL */
	char *layout;      /* may be NULL */
	char *archive;     /* relative filename, e.g. "peacock-shell-0.1.0.feather" */
	char *sha256;      /* lowercase-hex; may be NULL if absent */
	long long size;    /* -1 if absent */
	/* repo name this entry came from (borrowed pointer into the
	 * caller-owned ftr_repo_index that contains it). */
	const char *repo_name;
} ftr_pkg_index_entry;

typedef struct {
	char *repo_name;                /* heap copy */
	ftr_pkg_index_entry *entries;
	size_t n_entries;
} ftr_repo_index;

/* Effective config path: honors $FTR_CONFIG, falls back to
 * FTR_CONFIG compile-time default. Returned pointer is statically
 * owned and stays valid until the next call. */
const char *ftr_config_path(void);

/* Parse feather.conf into a list of repos. Returns 0 on success
 * (incl. "file not present" → empty list), -1 on parse error.
 * Caller frees with ftr_repo_list_free(). */
int ftr_repo_load_config(ftr_repo_list *out, char *errbuf, size_t errbufsz);

void ftr_repo_list_free(ftr_repo_list *l);

/* Load a single repo index.toml into a parsed structure. */
int ftr_repo_index_load(const char *path, const char *repo_name,
                        ftr_repo_index *out,
                        char *errbuf, size_t errbufsz);

void ftr_repo_index_free(ftr_repo_index *idx);

/* Load every index under $FTR_DB_ROOT/sync/. Returns 0 on success;
 * on success *out_arr is a heap array of ftr_repo_index (caller frees
 * each + the array via ftr_repo_indexes_free()). When no repos are
 * synced, returns 0 with *out_arr=NULL and *n_out=0. */
int ftr_repo_load_all_synced(ftr_repo_index **out_arr, size_t *n_out);

void ftr_repo_indexes_free(ftr_repo_index *arr, size_t n);

/* Copy a file from `src_url` to `dst_path`. Supports `file://` URLs
 * only; for other schemes, sets *skipped to 1 (with a phase-6
 * stdout/stderr line) and returns 0 without writing anything.
 * Returns 0 on success, -1 on hard I/O error. */
int ftr_repo_fetch(const char *src_url, const char *dst_path,
                   int *skipped);

/* Strip a leading "file://" from `url`. Returns a pointer into `url`
 * (no allocation) if the prefix is present; otherwise returns NULL. */
const char *ftr_repo_file_url_path(const char *url);

/* True if `url` begins with `file://`. */
int ftr_repo_is_file_url(const char *url);

#endif /* FTR_REPO_H */
