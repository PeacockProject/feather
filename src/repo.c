/*
 * repo.c — repository config + index reader + file:// fetch.
 *
 * Uses the vendored tomlc99 parser for both feather.conf and per-repo
 * index.toml. File transfers are byte-copy through the existing
 * copy_file() helper — no libcurl, no network code in this phase.
 *
 * URL handling is deliberately minimalist: anything that isn't
 * `file://` prints a one-line "phase 6 will add this" diagnostic and
 * is skipped rather than failing the whole call. Sync is best-effort.
 */

#define _POSIX_C_SOURCE 200809L

#include "repo.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "common.h"
#include "db.h"
#include "toml.h"
#include "util.h"

/* ---------------------------------------------------------------- *
 * config path resolution
 * ---------------------------------------------------------------- */

const char *ftr_config_path(void)
{
	static char cached[4096];
	const char *env = getenv("FTR_CONFIG");
	const char *src = (env && *env) ? env : FTR_CONFIG;
	(void)snprintf(cached, sizeof(cached), "%s", src);
	cached[sizeof(cached) - 1] = '\0';
	return cached;
}

/* ---------------------------------------------------------------- *
 * url helpers
 * ---------------------------------------------------------------- */

int ftr_repo_is_file_url(const char *url)
{
	return url && strncmp(url, "file://", 7) == 0;
}

const char *ftr_repo_file_url_path(const char *url)
{
	if (!ftr_repo_is_file_url(url)) {
		return NULL;
	}
	return url + 7;
}

/* ---------------------------------------------------------------- *
 * feather.conf parser
 * ---------------------------------------------------------------- */

static void set_err(char *errbuf, size_t errbufsz, const char *fmt, ...)
{
	va_list ap;
	if (!errbuf || errbufsz == 0) {
		return;
	}
	va_start(ap, fmt);
	(void)vsnprintf(errbuf, errbufsz, fmt, ap);
	va_end(ap);
	errbuf[errbufsz - 1] = '\0';
}

void ftr_repo_list_free(ftr_repo_list *l)
{
	size_t i;
	if (!l) {
		return;
	}
	for (i = 0; i < l->n; i++) {
		free(l->items[i].name);
		free(l->items[i].url);
	}
	free(l->items);
	l->items = NULL;
	l->n = 0;
}

int ftr_repo_load_config(ftr_repo_list *out, char *errbuf, size_t errbufsz)
{
	FILE *fp;
	toml_table_t *root = NULL;
	toml_array_t *arr;
	char toml_err[256];
	int n;
	int i;
	const char *path = ftr_config_path();

	memset(out, 0, sizeof(*out));

	fp = fopen(path, "r");
	if (!fp) {
		if (errno == ENOENT) {
			return 0; /* no config => no repos, not an error */
		}
		set_err(errbuf, errbufsz, "cannot open config '%s'", path);
		return -1;
	}
	root = toml_parse_file(fp, toml_err, sizeof(toml_err));
	fclose(fp);
	if (!root) {
		set_err(errbuf, errbufsz, "config parse error: %s", toml_err);
		return -1;
	}

	arr = toml_array_in(root, "repos");
	if (!arr) {
		toml_free(root);
		return 0;
	}
	n = toml_array_nelem(arr);
	if (n <= 0) {
		toml_free(root);
		return 0;
	}
	out->items = calloc((size_t)n, sizeof(*out->items));
	if (!out->items) {
		toml_free(root);
		set_err(errbuf, errbufsz, "out of memory");
		return -1;
	}
	for (i = 0; i < n; i++) {
		toml_table_t *t = toml_table_at(arr, i);
		toml_datum_t name_d, url_d;
		if (!t) {
			set_err(errbuf, errbufsz,
			        "[[repos]] entry #%d is not a table", i);
			goto fail;
		}
		name_d = toml_string_in(t, "name");
		url_d  = toml_string_in(t, "url");
		if (!name_d.ok || !url_d.ok) {
			if (name_d.ok) free(name_d.u.s);
			if (url_d.ok)  free(url_d.u.s);
			set_err(errbuf, errbufsz,
			        "[[repos]] entry #%d missing name or url", i);
			goto fail;
		}
		out->items[i].name = name_d.u.s;
		out->items[i].url  = url_d.u.s;
		out->n++;
	}

	toml_free(root);
	return 0;

fail:
	toml_free(root);
	ftr_repo_list_free(out);
	return -1;
}

/* ---------------------------------------------------------------- *
 * index.toml parser
 * ---------------------------------------------------------------- */

void ftr_repo_index_free(ftr_repo_index *idx)
{
	size_t i;
	if (!idx) {
		return;
	}
	for (i = 0; i < idx->n_entries; i++) {
		free(idx->entries[i].name);
		free(idx->entries[i].version);
		free(idx->entries[i].description);
		free(idx->entries[i].runtime);
		free(idx->entries[i].layout);
		free(idx->entries[i].archive);
		free(idx->entries[i].sha256);
	}
	free(idx->entries);
	free(idx->repo_name);
	memset(idx, 0, sizeof(*idx));
}

static void take_opt_str(toml_table_t *t, const char *key, char **out)
{
	toml_datum_t d = toml_string_in(t, key);
	if (d.ok) {
		*out = d.u.s;
	}
}

static long long take_opt_int(toml_table_t *t, const char *key)
{
	toml_datum_t d = toml_int_in(t, key);
	if (d.ok) {
		return d.u.i;
	}
	return -1;
}

int ftr_repo_index_load(const char *path, const char *repo_name,
                        ftr_repo_index *out,
                        char *errbuf, size_t errbufsz)
{
	FILE *fp;
	toml_table_t *root = NULL;
	toml_array_t *arr;
	char toml_err[256];
	int n;
	int i;

	memset(out, 0, sizeof(*out));

	fp = fopen(path, "r");
	if (!fp) {
		set_err(errbuf, errbufsz, "cannot open index '%s'", path);
		return -1;
	}
	root = toml_parse_file(fp, toml_err, sizeof(toml_err));
	fclose(fp);
	if (!root) {
		set_err(errbuf, errbufsz, "index parse error: %s", toml_err);
		return -1;
	}

	out->repo_name = strdup(repo_name);
	if (!out->repo_name) {
		set_err(errbuf, errbufsz, "out of memory");
		toml_free(root);
		return -1;
	}

	arr = toml_array_in(root, "package");
	if (!arr) {
		toml_free(root);
		return 0;
	}
	n = toml_array_nelem(arr);
	if (n <= 0) {
		toml_free(root);
		return 0;
	}
	out->entries = calloc((size_t)n, sizeof(*out->entries));
	if (!out->entries) {
		set_err(errbuf, errbufsz, "out of memory");
		toml_free(root);
		ftr_repo_index_free(out);
		return -1;
	}
	for (i = 0; i < n; i++) {
		toml_table_t *t = toml_table_at(arr, i);
		toml_datum_t name_d, ver_d, arch_d;
		if (!t) {
			set_err(errbuf, errbufsz,
			        "[[package]] #%d is not a table", i);
			goto fail;
		}
		name_d = toml_string_in(t, "name");
		ver_d  = toml_string_in(t, "version");
		arch_d = toml_string_in(t, "archive");
		if (!name_d.ok || !ver_d.ok || !arch_d.ok) {
			if (name_d.ok) free(name_d.u.s);
			if (ver_d.ok)  free(ver_d.u.s);
			if (arch_d.ok) free(arch_d.u.s);
			set_err(errbuf, errbufsz,
			        "[[package]] #%d missing name/version/archive",
			        i);
			goto fail;
		}
		out->entries[i].name    = name_d.u.s;
		out->entries[i].version = ver_d.u.s;
		out->entries[i].archive = arch_d.u.s;
		take_opt_str(t, "description", &out->entries[i].description);
		take_opt_str(t, "runtime",     &out->entries[i].runtime);
		take_opt_str(t, "layout",      &out->entries[i].layout);
		take_opt_str(t, "sha256",      &out->entries[i].sha256);
		out->entries[i].size = take_opt_int(t, "size");
		out->entries[i].repo_name = out->repo_name;
		out->n_entries++;
	}

	toml_free(root);
	return 0;

fail:
	toml_free(root);
	ftr_repo_index_free(out);
	return -1;
}

/* ---------------------------------------------------------------- *
 * load every synced index
 * ---------------------------------------------------------------- */

void ftr_repo_indexes_free(ftr_repo_index *arr, size_t n)
{
	size_t i;
	if (!arr) {
		return;
	}
	for (i = 0; i < n; i++) {
		ftr_repo_index_free(&arr[i]);
	}
	free(arr);
}

int ftr_repo_load_all_synced(ftr_repo_index **out_arr, size_t *n_out)
{
	char *sync_root = path_join(2, ftr_db_root(), "sync");
	DIR *d;
	struct dirent *de;
	ftr_repo_index *buf = NULL;
	size_t cap = 0;
	size_t n = 0;

	*out_arr = NULL;
	*n_out = 0;
	if (!sync_root) {
		return -1;
	}
	d = opendir(sync_root);
	if (!d) {
		int e = errno;
		free(sync_root);
		if (e == ENOENT) {
			return 0;
		}
		return -1;
	}
	while ((de = readdir(d)) != NULL) {
		char *idx_path;
		char err[256];
		ftr_repo_index idx;
		if (de->d_name[0] == '.') {
			continue;
		}
		idx_path = path_join(3, sync_root, de->d_name, "index.toml");
		if (!idx_path) {
			closedir(d); free(sync_root);
			ftr_repo_indexes_free(buf, n);
			return -1;
		}
		if (!path_exists(idx_path)) {
			free(idx_path);
			continue;
		}
		if (ftr_repo_index_load(idx_path, de->d_name, &idx,
		                        err, sizeof(err)) != 0) {
			err_log("repo: %s: %s", de->d_name, err);
			free(idx_path);
			continue;
		}
		free(idx_path);
		if (n == cap) {
			size_t nc = (cap == 0) ? 4 : cap * 2;
			ftr_repo_index *tmp =
			        realloc(buf, nc * sizeof(*tmp));
			if (!tmp) {
				ftr_repo_index_free(&idx);
				closedir(d); free(sync_root);
				ftr_repo_indexes_free(buf, n);
				return -1;
			}
			buf = tmp;
			cap = nc;
		}
		buf[n++] = idx;
	}
	closedir(d);
	free(sync_root);
	*out_arr = buf;
	*n_out = n;
	return 0;
}

/* ---------------------------------------------------------------- *
 * fetch
 * ---------------------------------------------------------------- */

int ftr_repo_fetch(const char *src_url, const char *dst_path, int *skipped)
{
	if (skipped) {
		*skipped = 0;
	}
	if (!ftr_repo_is_file_url(src_url)) {
		info_log("repo: skipping '%s' — scheme not yet supported "
		         "(phase 6 will add https)", src_url);
		if (skipped) {
			*skipped = 1;
		}
		return 0;
	}
	{
		const char *src = ftr_repo_file_url_path(src_url);
		if (!src) {
			return -1;
		}
		if (copy_file(src, dst_path) != 0) {
			err_log("repo: cannot copy '%s' -> '%s': %s",
			        src, dst_path, strerror(errno));
			return -1;
		}
		return 0;
	}
}
