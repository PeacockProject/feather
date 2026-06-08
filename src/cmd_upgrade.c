/*
 * cmd_upgrade.c — `ftr upgrade [<name>]`
 *
 * Walk the local DB; for each installed package (or just the named
 * one), look up its best version in the synced indexes. If the index
 * version is strictly higher than the installed version, re-invoke
 * the install path on it.
 *
 * Atomicity is out of scope here — phase 5 promises a proper
 * transactional upgrade; phase 4b is happy with "ftr install ...
 * overwrites".
 */

#define _POSIX_C_SOURCE 200809L

#include "common.h"
#include "db.h"
#include "install.h"
#include "repo.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_help(void)
{
	printf("Usage: ftr upgrade [options] [<pkg-name>]\n");
	printf("\n");
	printf("Upgrade installed packages to the highest version found in\n");
	printf("synced repos. With no name, every installed package is\n");
	printf("considered.\n");
	printf("\n");
	printf("Options:\n");
	printf("  --peacock-prefix <path>   override /peacock prefix\n");
	printf("  --apps-prefix <path>      override /apps prefix\n");
	printf("  --compat-prefix <path>    override /compat prefix\n");
	printf("  -h, --help                show this help\n");
	printf("\n");
	printf("Phase 4b: re-invokes the install path on each upgrade\n");
	printf("target — atomic transaction lands in phase 5.\n");
}

/* Find the best (highest-version, first-listed-repo on tie) repo
 * entry for `name`. */
static const ftr_pkg_index_entry *
find_best(const ftr_repo_index *idxs, size_t n_idxs, const char *name)
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

/* Split "name-version" at the last '-'; out_name/out_version are
 * heap-owned. Returns 0 on success, -1 if no '-' present. */
static int split_combo(const char *combo,
                       char **out_name, char **out_version)
{
	const char *dash = strrchr(combo, '-');
	if (!dash || dash == combo) {
		return -1;
	}
	*out_name    = strndup(combo, (size_t)(dash - combo));
	*out_version = strdup(dash + 1);
	if (!*out_name || !*out_version) {
		free(*out_name);
		free(*out_version);
		return -1;
	}
	return 0;
}

/* Consider one installed package. Returns 0 on success (incl.
 * "up to date" / "no candidate"); -1 on hard error. */
static int upgrade_one(const ftr_repo_index *idxs, size_t n_idxs,
                       const char *combo,
                       const ftr_install_opts *opts)
{
	char *name = NULL;
	char *version = NULL;
	const ftr_pkg_index_entry *best;
	int rc = -1;

	if (split_combo(combo, &name, &version) != 0) {
		err_log("upgrade: malformed DB entry '%s'", combo);
		return -1;
	}

	best = find_best(idxs, n_idxs, name);
	if (!best) {
		printf("%s: not in any synced repo\n", name);
		rc = 0;
		goto out;
	}
	if (version_compare(best->version, version) <= 0) {
		printf("%s: up to date\n", name);
		rc = 0;
		goto out;
	}

	printf("%s: %s -> %s\n", name, version, best->version);
	if (ftr_install_by_name(name, opts) != 0) {
		err_log("upgrade: install of '%s' failed", name);
		goto out;
	}
	rc = 0;

out:
	free(name);
	free(version);
	return rc;
}

int cmd_upgrade(int argc, char **argv)
{
	ftr_install_opts opts;
	ftr_repo_index *idxs = NULL;
	size_t n_idxs = 0;
	char **installed = NULL;
	size_t n_installed = 0;
	const char *target = NULL;
	size_t i;
	int rc = 0;
	int found = 0;
	int ai;

	memset(&opts, 0, sizeof(opts));

	/* Parse flags. Stop at the first positional. */
	for (ai = 1; ai < argc; ai++) {
		const char *a = argv[ai];
		if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
			print_help();
			return 0;
		}
		if (strcmp(a, "--peacock-prefix") == 0) {
			if (++ai >= argc) {
				err_log("upgrade: --peacock-prefix needs an "
				        "argument");
				return 2;
			}
			opts.peacock_prefix = argv[ai];
		} else if (strcmp(a, "--apps-prefix") == 0) {
			if (++ai >= argc) {
				err_log("upgrade: --apps-prefix needs an "
				        "argument");
				return 2;
			}
			opts.apps_prefix = argv[ai];
		} else if (strcmp(a, "--compat-prefix") == 0) {
			if (++ai >= argc) {
				err_log("upgrade: --compat-prefix needs an "
				        "argument");
				return 2;
			}
			opts.compat_prefix = argv[ai];
		} else if (a[0] == '-' && a[1] != '\0') {
			err_log("upgrade: unknown option '%s'", a);
			return 2;
		} else {
			break;
		}
	}
	if (ai < argc) {
		if (ai + 1 < argc) {
			err_log("upgrade: takes at most one package name");
			return 2;
		}
		target = argv[ai];
	}

	if (ftr_repo_load_all_synced(&idxs, &n_idxs) != 0) {
		err_log("upgrade: cannot read synced repos");
		return 1;
	}
	if (n_idxs == 0) {
		info_log("upgrade: no synced repos — run 'ftr sync' first");
		ftr_repo_indexes_free(idxs, n_idxs);
		return 0;
	}

	if (ftr_db_list(&installed, &n_installed) != 0) {
		err_log("upgrade: cannot read DB");
		ftr_repo_indexes_free(idxs, n_idxs);
		return 1;
	}
	if (n_installed == 0) {
		info_log("upgrade: no installed packages");
		ftr_repo_indexes_free(idxs, n_idxs);
		return 0;
	}

	for (i = 0; i < n_installed; i++) {
		const char *combo = installed[i];
		if (target) {
			size_t tlen = strlen(target);
			if (strncmp(combo, target, tlen) != 0 ||
			    combo[tlen] != '-') {
				continue;
			}
			found = 1;
		}
		if (upgrade_one(idxs, n_idxs, combo, &opts) != 0) {
			rc = 1;
		}
	}
	if (target && !found) {
		err_log("upgrade: '%s' is not installed", target);
		rc = 1;
	}

	for (i = 0; i < n_installed; i++) {
		free(installed[i]);
	}
	free(installed);
	ftr_repo_indexes_free(idxs, n_idxs);
	return rc;
}
