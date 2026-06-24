/*
 * cmd_install.c — `ftr install [<options>] <pkg>...`
 *
 * Each positional is either a local archive (path with '/' or ending
 * in ".feather") or a bare package name. Bare names are resolved as a
 * single transaction against the synced repo indexes: dependencies are
 * pulled in (topological order, deps first), [conflicts] are rejected,
 * and already-installed packages are skipped. Each selected package is
 * fetched into a temp file, sha256- + signature-verified, then handed
 * to the local-install path.
 *
 * Options:
 *   --peacock-prefix <path>   override /peacock
 *   --apps-prefix <path>      override /apps
 *   --compat-prefix <path>    override /compat
 *   --data-prefix <path>      override /data (reserved; phase 5)
 *   --root <path>             layout=system: overlay into <path>/
 *   --arch <arch>             resolve only packages for this arch
 *   --allow-unsigned          local archives only: skip sig check
 *   --help                    one-liner usage
 *
 * On any error, abort with the diagnostic; partial installs are NOT
 * rolled back (cleanup via `ftr remove`).
 */

#define _POSIX_C_SOURCE 200809L

#include "common.h"
#include "install.h"
#include "repo.h"
#include "resolve.h"
#include "util.h"
#include "verify.h"
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
	printf("    indexes (run 'ftr sync' first). Dependencies are pulled\n");
	printf("    in automatically.\n");
	printf("\n");
	printf("Options:\n");
	printf("  --peacock-prefix <path>   override /peacock prefix\n");
	printf("  --apps-prefix <path>      override /apps prefix\n");
	printf("  --compat-prefix <path>    override /compat prefix\n");
	printf("  --root <path>             layout=system: overlay into <path>/ (build chroot)\n");
	printf("  --arch <arch>             resolve only packages for this arch\n");
	printf("  --data-prefix <path>      override /data prefix (phase 5)\n");
	printf("  --allow-unsigned          local archives only: skip sig check\n");
	printf("  -h, --help                show this help\n");
	printf("\n");
	printf("Signature handling:\n");
	printf("  - Repo packages: signature verification is mandatory.\n");
	printf("  - Local archives with a sidecar .sig: verified.\n");
	printf("  - Local archives without a .sig: installed with WARN.\n");
	printf("  - --allow-unsigned bypasses verification for local installs\n");
	printf("    only. It does not relax the repo path.\n");
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

/* Install one already-resolved index entry: locate its repo in `cfg`,
 * fetch the archive + signature, sha256- and signature-verify, then
 * overlay it. `cfg` is the loaded feather.conf (borrowed). 0/-1. */
static int install_resolved_entry(const ftr_pkg_index_entry *best,
                                  const ftr_repo_list *cfg,
                                  const ftr_install_opts *opts)
{
	const char *src_repo_url = NULL;
	const char *src_repo_pubkey = NULL;
	char *src_url = NULL;
	char *sig_url = NULL;
	char arch_tmpl[] = "/tmp/feather-fetch-XXXXXX";
	char sig_tmpl[]  = "/tmp/feather-fetch-XXXXXX";
	int tmp_fd = -1;
	char *arch_path = NULL;
	char *sig_path = NULL;
	ftr_signature sig;
	ftr_pubkey pk;
	char err[512];
	size_t i;
	int rc = -1;

	memset(&sig, 0, sizeof(sig));
	memset(&pk, 0, sizeof(pk));

	for (i = 0; i < cfg->n; i++) {
		if (strcmp(cfg->items[i].name, best->repo_name) == 0) {
			src_repo_url = cfg->items[i].url;
			src_repo_pubkey = cfg->items[i].pubkey;
			break;
		}
	}
	if (!src_repo_url) {
		err_log("install: repo '%s' not in feather.conf (stale "
		        "index?)", best->repo_name);
		return -1;
	}

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
		sig_url = malloc(len + 4);
		if (!sig_url) {
			err_log("install: out of memory");
			goto out;
		}
		(void)snprintf(sig_url, len + 4, "%s/%s.sig",
		               src_repo_url, best->archive);
	}

	tmp_fd = mkstemp(arch_tmpl);
	if (tmp_fd < 0) {
		err_log("install: mkstemp failed: %s", strerror(errno));
		goto out;
	}
	close(tmp_fd);
	tmp_fd = -1;
	arch_path = strdup(arch_tmpl);
	if (!arch_path) {
		err_log("install: out of memory");
		(void)unlink(arch_tmpl);
		goto out;
	}
	tmp_fd = mkstemp(sig_tmpl);
	if (tmp_fd < 0) {
		err_log("install: mkstemp failed: %s", strerror(errno));
		goto out;
	}
	close(tmp_fd);
	tmp_fd = -1;
	sig_path = strdup(sig_tmpl);
	if (!sig_path) {
		err_log("install: out of memory");
		(void)unlink(sig_tmpl);
		goto out;
	}

	if (ftr_repo_fetch(src_url, arch_path, 0, NULL,
	                   err, sizeof(err)) != 0) {
		err_log("install: %s", err);
		goto out;
	}
	if (ftr_repo_fetch(sig_url, sig_path, 0, NULL,
	                   err, sizeof(err)) != 0) {
		err_log("install: cannot fetch signature: %s", err);
		goto out;
	}

	if (best->sha256 && *best->sha256) {
		char hex[65];
		if (sha256_file_hex(arch_path, hex) != 0) {
			err_log("install: cannot hash '%s'", arch_path);
			goto out;
		}
		if (strcmp(hex, best->sha256) != 0) {
			err_log("install: sha256 mismatch for '%s' "
			        "(index=%s, got=%s)",
			        best->archive, best->sha256, hex);
			goto out;
		}
	}

	if (ftr_verify_resolve_pubkey(src_repo_pubkey, &pk,
	                              err, sizeof(err)) != 0) {
		err_log("install: cannot load pubkey for repo '%s': %s",
		        best->repo_name, err);
		goto out;
	}
	if (ftr_verify_load_signature(sig_path, &sig,
	                              err, sizeof(err)) != 0) {
		err_log("install: %s-%s: %s",
		        best->name, best->version, err);
		goto out;
	}
	if (ftr_verify_archive(arch_path, &sig, &pk,
	                       err, sizeof(err)) != 0) {
		char fp[17];
		char hex[65];
		ftr_pubkey_fingerprint(&pk, fp);
		if (sha256_file_hex(arch_path, hex) != 0) {
			(void)snprintf(hex, sizeof(hex), "(hash unavailable)");
		}
		err_log("install: signature verify failed for %s-%s: %s",
		        best->name, best->version, err);
		err_log("install: signer key fingerprint: %s", fp);
		err_log("install: computed sha256: %s "
		        "(index claims: %s)",
		        hex,
		        (best->sha256 && *best->sha256)
		            ? best->sha256 : "(none)");
		goto out;
	}

	{
		ftr_install_opts internal_opts;
		if (opts) {
			internal_opts = *opts;
		} else {
			memset(&internal_opts, 0, sizeof(internal_opts));
		}
		internal_opts._signature_already_verified = 1;
		if (ftr_install_local(arch_path, &internal_opts) != 0) {
			goto out;
		}
	}
	{
		char fp[17];
		ftr_pubkey_fingerprint(&pk, fp);
		printf("installed: %s-%s (verified by %s)\n",
		       best->name, best->version, fp);
	}
	rc = 0;

out:
	if (arch_path) {
		(void)unlink(arch_path);
		free(arch_path);
	}
	if (sig_path) {
		(void)unlink(sig_path);
		free(sig_path);
	}
	free(src_url);
	free(sig_url);
	return rc;
}

/* Resolve a single `name` (highest version, no dep resolution) and
 * install it. Kept for `ftr upgrade`. Returns 0/-1. */
int ftr_install_by_name(const char *name,
                        const ftr_install_opts *opts)
{
	ftr_repo_index *idxs = NULL;
	size_t n_idxs = 0;
	const ftr_pkg_index_entry *best;
	ftr_repo_list cfg;
	char cfg_err[256];
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
	if (ftr_repo_load_config(&cfg, cfg_err, sizeof(cfg_err)) != 0) {
		err_log("install: %s", cfg_err);
		goto out;
	}
	rc = install_resolved_entry(best, &cfg, opts);

out:
	ftr_repo_list_free(&cfg);
	ftr_repo_indexes_free(idxs, n_idxs);
	return rc;
}

/* Resolve `names` (+ deps) as one transaction and install them in
 * topological order. Returns 0/-1. */
static int install_names_transaction(const char *const *names, size_t n_names,
                                     const char *arch,
                                     const ftr_install_opts *opts)
{
	ftr_repo_index *idxs = NULL;
	size_t n_idxs = 0;
	ftr_repo_list cfg;
	char cfg_err[256];
	char rerr[512];
	ftr_resolve_result res;
	size_t k;
	int rc = -1;

	memset(&cfg, 0, sizeof(cfg));
	memset(&res, 0, sizeof(res));

	if (ftr_repo_load_all_synced(&idxs, &n_idxs) != 0) {
		err_log("install: cannot read synced repos");
		return -1;
	}
	if (n_idxs == 0) {
		err_log("install: no synced repos — run 'ftr sync' first");
		goto out;
	}
	if (ftr_resolve(names, n_names, idxs, n_idxs, arch,
	                &res, rerr, sizeof(rerr)) != 0) {
		err_log("install: %s", rerr);
		goto out;
	}
	if (res.n == 0) {
		printf("install: nothing to do (already installed)\n");
		rc = 0;
		goto out;
	}
	if (ftr_repo_load_config(&cfg, cfg_err, sizeof(cfg_err)) != 0) {
		err_log("install: %s", cfg_err);
		goto out;
	}

	printf("install: %zu package(s):", res.n);
	for (k = 0; k < res.n; k++) {
		printf(" %s", res.items[k]->name);
	}
	printf("\n");

	for (k = 0; k < res.n; k++) {
		if (install_resolved_entry(res.items[k], &cfg, opts) != 0) {
			goto out;
		}
	}
	rc = 0;

out:
	ftr_resolve_free(&res);
	ftr_repo_list_free(&cfg);
	ftr_repo_indexes_free(idxs, n_idxs);
	return rc;
}

int cmd_install(int argc, char **argv)
{
	ftr_install_opts opts;
	const char *arch = NULL;
	const char **names = NULL;
	size_t n_names = 0;
	int i;
	int targets = 0;
	int rc = 0;

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
		} else if (strcmp(a, "--root") == 0) {
			if (++i >= argc) {
				err_log("install: --root needs an argument");
				return 2;
			}
			opts.root = argv[i];
		} else if (strcmp(a, "--arch") == 0) {
			if (++i >= argc) {
				err_log("install: --arch needs an argument");
				return 2;
			}
			arch = argv[i];
		} else if (strcmp(a, "--allow-unsigned") == 0) {
			opts.allow_unsigned = 1;
		} else if (a[0] == '-' && a[1] != '\0') {
			err_log("install: unknown option '%s'", a);
			return 2;
		} else {
			break;
		}
	}

	if (i < argc) {
		names = calloc((size_t)(argc - i), sizeof(*names));
		if (!names) {
			err_log("install: out of memory");
			return 1;
		}
	}

	/* Local archives install directly (no resolution); bare names are
	 * collected for a single resolved transaction. */
	for (; i < argc; i++) {
		const char *arg = argv[i];
		targets++;
		if (looks_like_path(arg)) {
			if (ftr_install_local(arg, &opts) != 0) {
				rc = 1;
				goto done;
			}
		} else {
			names[n_names++] = arg;
		}
	}

	if (n_names > 0) {
		if (install_names_transaction(names, n_names, arch, &opts) != 0) {
			rc = 1;
			goto done;
		}
	}

	if (targets == 0) {
		print_help();
		rc = 1;
	}

done:
	free(names);
	return rc;
}
