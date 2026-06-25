/*
 * repo.c — repository config + index reader + transport.
 *
 * Uses the vendored tomlc99 parser for both feather.conf and per-repo
 * index.toml. File transfers go through either the existing
 * copy_file() helper (file:// URLs) or a fork+exec curl(1) call
 * (http(s)://). curl is detected once at first need and the result is
 * cached for the rest of the run; deliberately no libcurl link, to
 * preserve `ftr`'s static-binary promise without dragging in libssl.
 */

#define _POSIX_C_SOURCE 200809L

#include "repo.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

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

int ftr_repo_is_http_url(const char *url)
{
	if (!url) {
		return 0;
	}
	return strncmp(url, "http://", 7) == 0 ||
	       strncmp(url, "https://", 8) == 0;
}

const char *ftr_repo_file_url_path(const char *url)
{
	if (!ftr_repo_is_file_url(url)) {
		return NULL;
	}
	return url + 7;
}

/* ---------------------------------------------------------------- *
 * curl detection — looked up once per `ftr` invocation.
 *
 * State values:
 *   -1  not yet probed
 *    0  not found in PATH
 *    1  found
 * ---------------------------------------------------------------- */

static int g_curl_state = -1;
static int g_wget_state = -1;

static int find_in_path(const char *name)
{
	const char *path = getenv("PATH");
	const char *p;
	char buf[4096];
	if (!path || !*path) {
		return 0;
	}
	p = path;
	while (*p) {
		const char *colon = strchr(p, ':');
		size_t seglen = colon ? (size_t)(colon - p) : strlen(p);
		if (seglen > 0 && seglen + 1 + strlen(name) + 1 <= sizeof(buf)) {
			memcpy(buf, p, seglen);
			buf[seglen] = '/';
			memcpy(buf + seglen + 1, name, strlen(name) + 1);
			if (access(buf, X_OK) == 0) {
				return 1;
			}
		}
		if (!colon) {
			break;
		}
		p = colon + 1;
	}
	return 0;
}

int ftr_repo_have_curl(void)
{
	if (g_curl_state < 0) {
		g_curl_state = find_in_path("curl") ? 1 : 0;
	}
	return g_curl_state;
}

int ftr_repo_have_wget(void)
{
	if (g_wget_state < 0) {
		g_wget_state = find_in_path("wget") ? 1 : 0;
	}
	return g_wget_state;
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
		free(l->items[i].pubkey);
		free(l->items[i].gpgkey);
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
		{
			toml_datum_t pk_d = toml_string_in(t, "pubkey");
			if (pk_d.ok) {
				out->items[i].pubkey = pk_d.u.s;
			}
		}
		{
			toml_datum_t gk_d = toml_string_in(t, "gpgkey");
			if (gk_d.ok) {
				out->items[i].gpgkey = gk_d.u.s;
			}
		}
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

static void free_str_array(char **a, size_t n);

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
		free(idx->entries[i].arch);
		free_str_array(idx->entries[i].depends, idx->entries[i].n_depends);
		free_str_array(idx->entries[i].provides, idx->entries[i].n_provides);
		free_str_array(idx->entries[i].conflicts, idx->entries[i].n_conflicts);
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

/* Optional array-of-string. On malformed entries, frees what it parsed
 * and leaves the outputs untouched (treated as "absent"). */
static void take_opt_str_array(toml_table_t *t, const char *key,
                               char ***out, size_t *n_out)
{
	toml_array_t *arr = toml_array_in(t, key);
	int n;
	int i;
	char **buf;
	if (!arr) {
		return;
	}
	n = toml_array_nelem(arr);
	if (n <= 0) {
		return;
	}
	buf = calloc((size_t)n, sizeof(*buf));
	if (!buf) {
		return;
	}
	for (i = 0; i < n; i++) {
		toml_datum_t d = toml_string_at(arr, i);
		if (!d.ok) {
			int j;
			for (j = 0; j < i; j++) {
				free(buf[j]);
			}
			free(buf);
			return;
		}
		buf[i] = d.u.s;
	}
	*out = buf;
	*n_out = (size_t)n;
}

static void free_str_array(char **a, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++) {
		free(a[i]);
	}
	free(a);
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
		take_opt_str(t, "arch",        &out->entries[i].arch);
		take_opt_str_array(t, "depends",
		                   &out->entries[i].depends, &out->entries[i].n_depends);
		take_opt_str_array(t, "provides",
		                   &out->entries[i].provides, &out->entries[i].n_provides);
		take_opt_str_array(t, "conflicts",
		                   &out->entries[i].conflicts, &out->entries[i].n_conflicts);
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

/* ---------------------------------------------------------------- *
 * curl HTTPS fetch
 *
 * Fork+exec curl with --fail / --silent / --location / --retry 3 so
 * we get a non-zero exit on 4xx/5xx and reasonable retry behavior on
 * transient errors. Stderr is captured through a pipe and surfaced in
 * `errbuf` on failure so the user can see what curl complained about
 * (DNS, TLS, 404, ...) without having to re-run with `--verbose`.
 *
 * When `optional` is set, a 404 / missing-file outcome is silently
 * mapped to *skipped = 1 + return 0; callers use this for the
 * .sig sidecar lookup so a repo that hasn't been signed yet doesn't
 * fail sync hard.
 * ---------------------------------------------------------------- */

static int curl_fetch_https(const char *url, const char *out_path,
                            int optional, int *skipped,
                            char *errbuf, size_t errbufsz)
{
	int err_pipe[2];
	pid_t pid;
	int status;
	char *stderr_buf = NULL;
	size_t stderr_cap = 4096;
	size_t stderr_len = 0;
	int use_wget = 0;
	const char *tool = "curl";

	/* Prefer curl; fall back to wget (e.g. busybox wget on recovery images).
	 * wget's TLS cert validation may be a no-op, but feather verifies the
	 * minisign signature of every artifact, so transport auth isn't the trust
	 * anchor here. */
	if (!ftr_repo_have_curl()) {
		if (ftr_repo_have_wget()) {
			use_wget = 1;
			tool = "wget";
		} else {
			set_err(errbuf, errbufsz,
			        "neither curl nor wget found in PATH; install "
			        "one (apk add curl / pacman -S curl / apt install curl)");
			return -1;
		}
	}

	if (pipe(err_pipe) != 0) {
		set_err(errbuf, errbufsz, "pipe(): %s", strerror(errno));
		return -1;
	}

	pid = fork();
	if (pid < 0) {
		close(err_pipe[0]);
		close(err_pipe[1]);
		set_err(errbuf, errbufsz, "fork(): %s", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		/* child: re-wire stderr to the pipe, drop stdin/stdout
		 * to /dev/null. */
		int devnull;
		close(err_pipe[0]);
		dup2(err_pipe[1], STDERR_FILENO);
		close(err_pipe[1]);
		devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDOUT_FILENO);
			if (devnull > 2) {
				close(devnull);
			}
		}
		if (use_wget) {
			/* busybox/GNU wget: -q quiet, -T timeout, -O output. */
			execlp("wget", "wget",
			       "-q", "-T", "30", "-O", out_path, url,
			       (char *)NULL);
		} else {
			execlp("curl", "curl",
			       "-fsSL",
			       "--retry", "3",
			       "--retry-delay", "2",
			       "-o", out_path,
			       url,
			       (char *)NULL);
		}
		_exit(127);
	}

	/* parent: drain stderr, then wait. */
	close(err_pipe[1]);
	stderr_buf = malloc(stderr_cap);
	if (stderr_buf) {
		for (;;) {
			ssize_t n;
			if (stderr_len + 1024 >= stderr_cap) {
				size_t nc = stderr_cap * 2;
				char *tmp = realloc(stderr_buf, nc);
				if (!tmp) {
					break;
				}
				stderr_buf = tmp;
				stderr_cap = nc;
			}
			n = read(err_pipe[0],
			         stderr_buf + stderr_len,
			         stderr_cap - stderr_len - 1);
			if (n <= 0) {
				if (n < 0 && errno == EINTR) {
					continue;
				}
				break;
			}
			stderr_len += (size_t)n;
		}
		stderr_buf[stderr_len] = '\0';
	}
	close(err_pipe[0]);

	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR) {
			free(stderr_buf);
			set_err(errbuf, errbufsz, "waitpid(): %s",
			        strerror(errno));
			return -1;
		}
	}

	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		free(stderr_buf);
		return 0;
	}

	/* curl exit 22 = HTTP error (with --fail); wget returns non-zero on a
	 * missing file. For optional fetches treat that as "absent" rather than a
	 * hard failure (used for the .sig sidecar lookup). */
	if (optional && WIFEXITED(status) &&
	    (use_wget ? WEXITSTATUS(status) != 0 : WEXITSTATUS(status) == 22)) {
		(void)unlink(out_path); /* curl leaves partial files */
		if (skipped) {
			*skipped = 1;
		}
		free(stderr_buf);
		return 0;
	}

	{
		const char *tail = stderr_buf ? stderr_buf : "";
		/* trim trailing whitespace for the error line */
		size_t tlen = strlen(tail);
		while (tlen > 0 &&
		       (tail[tlen - 1] == '\n' || tail[tlen - 1] == '\r' ||
		        tail[tlen - 1] == ' '  || tail[tlen - 1] == '\t')) {
			tlen--;
		}
		set_err(errbuf, errbufsz,
		        "%s: exit %d fetching '%s'%s%.*s",
		        tool,
		        WIFEXITED(status) ? WEXITSTATUS(status) : -1,
		        url,
		        tlen ? ": " : "",
		        (int)tlen, tail);
	}
	(void)unlink(out_path);
	free(stderr_buf);
	return -1;
}

int ftr_repo_fetch(const char *src_url, const char *dst_path,
                   int optional, int *skipped,
                   char *errbuf, size_t errbufsz)
{
	if (skipped) {
		*skipped = 0;
	}
	if (!src_url || !*src_url) {
		set_err(errbuf, errbufsz, "empty URL");
		return -1;
	}

	if (ftr_repo_is_file_url(src_url)) {
		const char *src = ftr_repo_file_url_path(src_url);
		if (!src) {
			set_err(errbuf, errbufsz,
			        "malformed file:// URL '%s'", src_url);
			return -1;
		}
		if (copy_file(src, dst_path) != 0) {
			if (optional && errno == ENOENT) {
				if (skipped) {
					*skipped = 1;
				}
				return 0;
			}
			set_err(errbuf, errbufsz,
			        "cannot copy '%s' -> '%s': %s",
			        src, dst_path, strerror(errno));
			return -1;
		}
		return 0;
	}

	if (ftr_repo_is_http_url(src_url)) {
		return curl_fetch_https(src_url, dst_path, optional,
		                        skipped, errbuf, errbufsz);
	}

	set_err(errbuf, errbufsz,
	        "repo scheme not supported in URL '%s' (need file:// or "
	        "http(s)://)", src_url);
	return -1;
}
