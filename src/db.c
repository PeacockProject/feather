/*
 * db.c — local install database at <DB_ROOT>/local/<name>-<version>/.
 *
 * Format is text-files-in-a-directory, like pacman. Each entry dir
 * carries:
 *
 *   manifest.toml      verbatim copy of the package's manifest
 *   files              newline-separated, sorted, paths relative to /
 *   installed.txt      ISO 8601 timestamp ("%Y-%m-%dT%H:%M:%SZ")
 *   hooks/             optional copy of the .feather hooks dir
 *
 * $FTR_DB_ROOT overrides FTR_DB_ROOT (compile-time default
 * /var/lib/feather), so tests can sandbox the DB.
 */

#define _POSIX_C_SOURCE 200809L

#include "db.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "util.h"

/* ---------------------------------------------------------------- *
 * root resolution
 * ---------------------------------------------------------------- */

const char *ftr_db_root(void)
{
	static char cached[4096];
	const char *env = getenv("FTR_DB_ROOT");
	const char *src = (env && *env) ? env : FTR_DB_ROOT;
	(void)snprintf(cached, sizeof(cached), "%s", src);
	cached[sizeof(cached) - 1] = '\0';
	return cached;
}

/* <DB_ROOT>/local — caller frees. */
static char *db_local_dir(void)
{
	return path_join(2, ftr_db_root(), "local");
}

/* <DB_ROOT>/local/<name>-<version> — caller frees. */
static char *entry_dir(const char *name, const char *version)
{
	char *local = db_local_dir();
	char *dir;
	char *combo;
	size_t nlen, vlen;
	if (!local) {
		return NULL;
	}
	nlen = strlen(name);
	vlen = strlen(version);
	combo = malloc(nlen + 1 + vlen + 1);
	if (!combo) {
		free(local);
		return NULL;
	}
	memcpy(combo, name, nlen);
	combo[nlen] = '-';
	memcpy(combo + nlen + 1, version, vlen + 1);
	dir = path_join(2, local, combo);
	free(local);
	free(combo);
	return dir;
}

/* ---------------------------------------------------------------- *
 * write helpers
 * ---------------------------------------------------------------- */

static int write_files_list(const char *path, char **files, size_t n)
{
	FILE *fp;
	size_t i;
	fp = fopen(path, "w");
	if (!fp) {
		return -1;
	}
	for (i = 0; i < n; i++) {
		if (fprintf(fp, "%s\n", files[i]) < 0) {
			fclose(fp);
			return -1;
		}
	}
	if (fclose(fp) != 0) {
		return -1;
	}
	return 0;
}

static int write_installed_at(const char *path)
{
	FILE *fp;
	time_t now = time(NULL);
	struct tm tm;
	char buf[64];

	if (gmtime_r(&now, &tm) == NULL) {
		return -1;
	}
	if (strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
		return -1;
	}
	fp = fopen(path, "w");
	if (!fp) {
		return -1;
	}
	if (fprintf(fp, "%s\n", buf) < 0) {
		fclose(fp);
		return -1;
	}
	if (fclose(fp) != 0) {
		return -1;
	}
	return 0;
}

/* ---------------------------------------------------------------- *
 * record
 * ---------------------------------------------------------------- */

int ftr_db_record(const char *name, const char *version,
                  const char *manifest_src_path,
                  const char *hooks_src_dir,
                  char **files, size_t n_files)
{
	char *dir = NULL;
	char *manifest_dst = NULL;
	char *files_path = NULL;
	char *installed_path = NULL;
	char *hooks_dst = NULL;
	int rc = -1;

	dir = entry_dir(name, version);
	if (!dir) {
		err_log("db: out of memory");
		goto out;
	}
	/* Re-install / upgrade semantics for phase 4: blow away every
	 * prior entry for this package name (any version), not just the
	 * matching (name, version) tuple. Phase 5 will model upgrades as
	 * a proper transactional swap; here we keep one version per
	 * name. */
	{
		char *existing_version = ftr_db_find_version(name);
		while (existing_version) {
			if (ftr_db_remove(name, existing_version) != 0) {
				err_log("db: cannot clear stale entry "
				        "%s-%s: %s",
				        name, existing_version,
				        strerror(errno));
				free(existing_version);
				goto out;
			}
			free(existing_version);
			existing_version = ftr_db_find_version(name);
		}
	}
	if (mkdir_p(dir, 0755) != 0) {
		err_log("db: cannot create '%s': %s", dir, strerror(errno));
		goto out;
	}

	manifest_dst = path_join(2, dir, "manifest.toml");
	files_path = path_join(2, dir, "files");
	installed_path = path_join(2, dir, "installed.txt");
	if (!manifest_dst || !files_path || !installed_path) {
		err_log("db: out of memory");
		goto out;
	}
	if (copy_file(manifest_src_path, manifest_dst) != 0) {
		err_log("db: cannot copy manifest into DB: %s",
		        strerror(errno));
		goto out;
	}
	if (write_files_list(files_path, files, n_files) != 0) {
		err_log("db: cannot write files list: %s", strerror(errno));
		goto out;
	}
	if (write_installed_at(installed_path) != 0) {
		err_log("db: cannot write timestamp: %s", strerror(errno));
		goto out;
	}

	if (hooks_src_dir && is_dir(hooks_src_dir)) {
		hooks_dst = path_join(2, dir, "hooks");
		if (!hooks_dst) {
			err_log("db: out of memory");
			goto out;
		}
		if (copy_tree(hooks_src_dir, hooks_dst) != 0) {
			err_log("db: cannot copy hooks: %s",
			        strerror(errno));
			goto out;
		}
	}

	rc = 0;
out:
	free(dir);
	free(manifest_dst);
	free(files_path);
	free(installed_path);
	free(hooks_dst);
	return rc;
}

/* ---------------------------------------------------------------- *
 * list / find / get
 * ---------------------------------------------------------------- */

int ftr_db_list(char ***names_out, size_t *n_out)
{
	char *local = db_local_dir();
	DIR *d;
	struct dirent *de;
	char **buf = NULL;
	size_t cap = 0;
	size_t n = 0;

	*names_out = NULL;
	*n_out = 0;
	if (!local) {
		return -1;
	}
	d = opendir(local);
	if (!d) {
		int e = errno;
		free(local);
		if (e == ENOENT) {
			return 0; /* empty DB */
		}
		return -1;
	}
	while ((de = readdir(d)) != NULL) {
		char *copy;
		if (de->d_name[0] == '.') {
			continue;
		}
		if (n == cap) {
			size_t new_cap = (cap == 0) ? 16 : cap * 2;
			char **tmp = realloc(buf, new_cap * sizeof(*tmp));
			if (!tmp) {
				goto oom;
			}
			buf = tmp;
			cap = new_cap;
		}
		copy = strdup(de->d_name);
		if (!copy) {
			goto oom;
		}
		buf[n++] = copy;
	}
	closedir(d);
	free(local);
	if (n > 0) {
		qsort(buf, n, sizeof(*buf), strcmp_qsort);
	}
	*names_out = buf;
	*n_out = n;
	return 0;

oom:
	{
		size_t i;
		for (i = 0; i < n; i++) {
			free(buf[i]);
		}
		free(buf);
	}
	closedir(d);
	free(local);
	return -1;
}

char *ftr_db_find_version(const char *name)
{
	char **all = NULL;
	size_t n = 0;
	size_t i;
	char *found = NULL;
	size_t name_len;

	if (ftr_db_list(&all, &n) != 0) {
		return NULL;
	}
	name_len = strlen(name);
	for (i = 0; i < n; i++) {
		if (strncmp(all[i], name, name_len) == 0 &&
		    all[i][name_len] == '-') {
			found = strdup(all[i] + name_len + 1);
			break;
		}
	}
	for (i = 0; i < n; i++) {
		free(all[i]);
	}
	free(all);
	return found;
}

/* Read a text file into a NUL-terminated heap buffer. Caller frees. */
static char *slurp(const char *path)
{
	FILE *fp = fopen(path, "r");
	char *buf;
	size_t cap = 4096;
	size_t len = 0;
	if (!fp) {
		return NULL;
	}
	buf = malloc(cap);
	if (!buf) {
		fclose(fp);
		return NULL;
	}
	for (;;) {
		size_t r;
		if (len + 1 >= cap) {
			char *tmp;
			cap *= 2;
			tmp = realloc(buf, cap);
			if (!tmp) {
				free(buf);
				fclose(fp);
				return NULL;
			}
			buf = tmp;
		}
		r = fread(buf + len, 1, cap - 1 - len, fp);
		len += r;
		if (r == 0) {
			break;
		}
	}
	fclose(fp);
	buf[len] = '\0';
	return buf;
}

int ftr_db_get(const char *name, const char *version, ftr_db_entry *out)
{
	char *dir;
	char *files_path = NULL;
	char *installed_path = NULL;
	char *files_buf = NULL;
	char *installed_buf = NULL;
	char *p;
	char **lines = NULL;
	size_t cap = 0;
	size_t n = 0;
	struct stat st;
	int rc = -1;

	memset(out, 0, sizeof(*out));

	dir = entry_dir(name, version);
	if (!dir) {
		return -1;
	}
	if (stat(dir, &st) != 0) {
		free(dir);
		return -1;
	}

	files_path = path_join(2, dir, "files");
	installed_path = path_join(2, dir, "installed.txt");
	if (!files_path || !installed_path) {
		goto out;
	}

	files_buf = slurp(files_path);
	if (!files_buf) {
		goto out;
	}
	installed_buf = slurp(installed_path);

	/* Split files_buf into newline-terminated entries. */
	p = files_buf;
	while (*p) {
		char *nl = strchr(p, '\n');
		size_t llen;
		char *line;
		if (nl) {
			llen = (size_t)(nl - p);
		} else {
			llen = strlen(p);
		}
		if (llen > 0) {
			if (n == cap) {
				size_t new_cap = (cap == 0) ? 16 : cap * 2;
				char **tmp = realloc(lines,
				                     new_cap * sizeof(*tmp));
				if (!tmp) {
					goto out;
				}
				lines = tmp;
				cap = new_cap;
			}
			line = malloc(llen + 1);
			if (!line) {
				goto out;
			}
			memcpy(line, p, llen);
			line[llen] = '\0';
			lines[n++] = line;
		}
		if (!nl) {
			break;
		}
		p = nl + 1;
	}

	out->name = strdup(name);
	out->version = strdup(version);
	if (!out->name || !out->version) {
		goto out;
	}
	if (installed_buf) {
		char *nl = strchr(installed_buf, '\n');
		if (nl) {
			*nl = '\0';
		}
		out->installed_at = strdup(installed_buf);
	}
	out->files = lines;
	out->n_files = n;
	lines = NULL; /* ownership moved */
	n = 0;
	rc = 0;

out:
	if (rc != 0 && lines) {
		size_t i;
		for (i = 0; i < n; i++) {
			free(lines[i]);
		}
		free(lines);
		ftr_db_entry_free(out);
	}
	free(files_buf);
	free(installed_buf);
	free(files_path);
	free(installed_path);
	free(dir);
	return rc;
}

void ftr_db_entry_free(ftr_db_entry *e)
{
	size_t i;
	if (!e) {
		return;
	}
	free(e->name);
	free(e->version);
	free(e->installed_at);
	for (i = 0; i < e->n_files; i++) {
		free(e->files[i]);
	}
	free(e->files);
	memset(e, 0, sizeof(*e));
}

char *ftr_db_hook_path(const char *name, const char *version,
                       const char *hook_name)
{
	char *dir = entry_dir(name, version);
	char hookfile[256];
	char *path;
	if (!dir) {
		return NULL;
	}
	(void)snprintf(hookfile, sizeof(hookfile), "%s.sh", hook_name);
	path = path_join(3, dir, "hooks", hookfile);
	free(dir);
	if (!path) {
		return NULL;
	}
	if (!path_exists(path)) {
		free(path);
		return NULL;
	}
	return path;
}

int ftr_db_remove(const char *name, const char *version)
{
	char *dir = entry_dir(name, version);
	int rc;
	if (!dir) {
		return -1;
	}
	rc = rm_rf(dir);
	free(dir);
	return rc;
}
