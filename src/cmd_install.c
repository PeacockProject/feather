/*
 * cmd_install.c — `ftr install [<options>] <pkg>...`
 *
 * Each positional is either a local archive (path with '/' or
 * ending in ".feather") or a bare package name resolved against the
 * synced repo indexes. Repo packages get fetched into a temp file,
 * sha256-verified against the index entry, then handed to the
 * existing local-install path.
 *
 * Options:
 *   --peacock-prefix <path>   override /peacock
 *   --apps-prefix <path>      override /apps
 *   --compat-prefix <path>    override /compat
 *   --data-prefix <path>      override /data (reserved; phase 5)
 *   --help                    one-liner usage
 *
 * On any error, abort with the diagnostic; partial installs are NOT
 * rolled back (cleanup via `ftr remove`).
 */

#define _POSIX_C_SOURCE 200809L

#include "common.h"
#include "install.h"
#include "repo.h"
#include "util.h"
#include "sha256.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_help(void)
{
	printf("Usage: ftr install [options] <pkg>...\n");
	printf("\n");
	printf("Install one or more packages. Each <pkg> may be either:\n");
	printf("  - a path to a local .feather archive (contains '/' or\n");
	printf("    ends in .feather), or\n");
	printf("  - a bare package name, resolved against synced repo\n");
	printf("    indexes (run 'ftr sync' first).\n");
	printf("\n");
	printf("Options:\n");
	printf("  --peacock-prefix <path>   override /peacock prefix\n");
	printf("  --apps-prefix <path>      override /apps prefix\n");
	printf("  --compat-prefix <path>    override /compat prefix\n");
	printf("  --data-prefix <path>      override /data prefix (phase 5)\n");
	printf("  -h, --help                show this help\n");
	printf("\n");
	printf("Note: repo packages install unsigned in phase 4 — signature\n");
	printf("verification is added in phase 6.\n");
}

/* Treat `arg` as a path (not a bare name) if it contains '/' or ends
 * in ".feather". */
static int looks_like_path(const char *arg)
{
	size_t n;
	if (!arg || !*arg) {
		return 0;
	}
	if (strchr(arg, '/') != NULL) {
		return 1;
	}
	n = strlen(arg);
	return n >= 8 && strcmp(arg + n - 8, ".feather") == 0;
}

/* Find the best (highest-version, first-listed-repo on tie) repo
 * entry for `name`. Returns NULL if no synced repo has it. The
 * returned pointer is borrowed from `idxs`. */
static const ftr_pkg_index_entry *resolve_pkg(const ftr_repo_index *idxs,
                                              size_t n_idxs,
                                              const char *name)
{
	const ftr_pkg_index_entry *best = NULL;
	size_t i;
	size_t j;
	for (i = 0; i < n_idxs; i++) {
		for (j = 0; j < idxs[i].n_entries; j++) {
			const ftr_pkg_index_entry *e = &idxs[i].entries[j];
			if (strcmp(e->name, name) != 0) {
				continue;
			}
			if (!best ||
			    version_compare(e->version, best->version) > 0) {
				best = e;
			}
		}
	}
	return best;
}

/* Resolve `name` from synced repos, fetch + verify + install. Returns
 * 0 on success, -1 on failure. */
static int install_from_repo(const char *name,
                             const ftr_install_opts *opts)
{
	ftr_repo_index *idxs = NULL;
	size_t n_idxs = 0;
	const ftr_pkg_index_entry *best;
	ftr_repo_list cfg;
	char cfg_err[256];
	const char *src_repo_url = NULL;
	char *src_url = NULL;
	char tmpl[] = "/tmp/feather-fetch-XXXXXX";
	int tmp_fd = -1;
	char *tmp_path = NULL;
	size_t i;
	int skipped = 0;
	int rc = -1;

	memset(&cfg, 0, sizeof(cfg));

	if (ftr_repo_load_all_synced(&idxs, &n_idxs) != 0) {
		err_log("install: cannot read synced repos");
		return -1;
	}
	if (n_idxs == 0) {
		err_log("install: no synced repos — run 'ftr sync' first");
		goto out;
	}
	best = resolve_pkg(idxs, n_idxs, name);
	if (!best) {
		err_log("install: package '%s' not found in any synced repo",
		        name);
		goto out;
	}

	/* Find this entry's repo URL from feather.conf so we know
	 * where to fetch the archive from. */
	if (ftr_repo_load_config(&cfg, cfg_err, sizeof(cfg_err)) != 0) {
		err_log("install: %s", cfg_err);
		goto out;
	}
	for (i = 0; i < cfg.n; i++) {
		if (strcmp(cfg.items[i].name, best->repo_name) == 0) {
			src_repo_url = cfg.items[i].url;
			break;
		}
	}
	if (!src_repo_url) {
		err_log("install: repo '%s' not in feather.conf (stale "
		        "index?)", best->repo_name);
		goto out;
	}

	/* "<url>/<archive>" */
	{
		size_t len = strlen(src_repo_url) + 1 + strlen(best->archive)
		             + 1;
		src_url = malloc(len);
		if (!src_url) {
			err_log("install: out of memory");
			goto out;
		}
		(void)snprintf(src_url, len, "%s/%s",
		               src_repo_url, best->archive);
	}

	/* mkstemp gives us a unique file under /tmp. install_local
	 * cares about contents, not the filename suffix. */
	tmp_fd = mkstemp(tmpl);
	if (tmp_fd < 0) {
		err_log("install: mkstemp failed: %s", strerror(errno));
		goto out;
	}
	close(tmp_fd);
	tmp_fd = -1;
	tmp_path = strdup(tmpl);
	if (!tmp_path) {
		err_log("install: out of memory");
		(void)unlink(tmpl);
		goto out;
	}

	if (ftr_repo_fetch(src_url, tmp_path, &skipped) != 0) {
		err_log("install: fetch '%s' failed", src_url);
		goto out;
	}
	if (skipped) {
		/* Non-file:// scheme — repo_fetch already logged. */
		err_log("install: cannot install '%s' from repo '%s': scheme "
		        "not yet supported", name, best->repo_name);
		goto out;
	}

	if (best->sha256 && *best->sha256) {
		char hex[65];
		if (sha256_file_hex(tmp_path, hex) != 0) {
			err_log("install: cannot hash '%s'", tmp_path);
			goto out;
		}
		if (strcmp(hex, best->sha256) != 0) {
			err_log("install: sha256 mismatch for '%s' "
			        "(index=%s, got=%s)",
			        best->archive, best->sha256, hex);
			goto out;
		}
	}

	if (ftr_install_local(tmp_path, opts) != 0) {
		goto out;
	}
	printf("installed: %s-%s (unsigned, phase 6 will sign)\n",
	       best->name, best->version);
	rc = 0;

out:
	if (tmp_path) {
		(void)unlink(tmp_path);
		free(tmp_path);
	}
	ftr_repo_list_free(&cfg);
	ftr_repo_indexes_free(idxs, n_idxs);
	free(src_url);
	return rc;
}

int cmd_install(int argc, char **argv)
{
	ftr_install_opts opts;
	int i;
	int targets = 0;

	memset(&opts, 0, sizeof(opts));

	/* Parse flags. Stop at the first positional. */
	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
			print_help();
			return 0;
		}
		if (strcmp(a, "--peacock-prefix") == 0) {
			if (++i >= argc) {
				err_log("install: --peacock-prefix needs an "
				        "argument");
				return 2;
			}
			opts.peacock_prefix = argv[i];
		} else if (strcmp(a, "--apps-prefix") == 0) {
			if (++i >= argc) {
				err_log("install: --apps-prefix needs an "
				        "argument");
				return 2;
			}
			opts.apps_prefix = argv[i];
		} else if (strcmp(a, "--compat-prefix") == 0) {
			if (++i >= argc) {
				err_log("install: --compat-prefix needs an "
				        "argument");
				return 2;
			}
			opts.compat_prefix = argv[i];
		} else if (strcmp(a, "--data-prefix") == 0) {
			if (++i >= argc) {
				err_log("install: --data-prefix needs an "
				        "argument");
				return 2;
			}
			opts.data_prefix = argv[i];
		} else if (a[0] == '-' && a[1] != '\0') {
			err_log("install: unknown option '%s'", a);
			return 2;
		} else {
			break;
		}
	}

	for (; i < argc; i++) {
		const char *arg = argv[i];
		targets++;
		if (looks_like_path(arg)) {
			if (ftr_install_local(arg, &opts) != 0) {
				return 1;
			}
		} else {
			if (install_from_repo(arg, &opts) != 0) {
				return 1;
			}
		}
	}

	if (targets == 0) {
		print_help();
		return 1;
	}
	return 0;
}
