/*
 * cmd_sync.c — `ftr sync`
 *
 * Read /etc/feather/feather.conf (or $FTR_CONFIG) and for each
 * [[repos]] entry fetch:
 *   1. <url>/index.toml      → $FTR_DB_ROOT/sync/<name>/index.toml
 *   2. <url>/index.toml.sig  → $FTR_DB_ROOT/sync/<name>/index.toml.sig
 *
 * The index signature is verified against the repo's pubkey (per-repo
 * override → $FTR_PUBKEY → built-in). On verify failure the previously
 * synced index/sig pair is kept and a WARN line is printed; the user
 * can still install whatever the old (good) index advertised.
 *
 * Supports file:// and http(s):// URLs. http(s) requires curl(1) in
 * PATH; if absent, fail-fast with an actionable error.
 */

#define _POSIX_C_SOURCE 200809L

#include "common.h"
#include "db.h"
#include "repo.h"
#include "util.h"
#include "verify.h"
#include "keyring.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void print_help(void)
{
	printf("Usage: ftr sync\n");
	printf("\n");
	printf("Refresh repository metadata. Reads %s\n", FTR_CONFIG);
	printf("(or $FTR_CONFIG) and fetches each repo's index.toml +\n");
	printf("index.toml.sig into $FTR_DB_ROOT/sync/<repo-name>/.\n");
	printf("\n");
	printf("Supported URL schemes: file://, http://, https://.\n");
	printf("The index signature is verified against the repo's pubkey\n");
	printf("(per-repo override, $FTR_PUBKEY, or compile-time default).\n");
	printf("On verify failure the previously synced index is kept.\n");
}

/* Build "<base>/<suffix>" into a freshly-malloc'd string. */
static char *join_url(const char *base, const char *suffix)
{
	size_t len = strlen(base) + 1 + strlen(suffix) + 1;
	char *u = malloc(len);
	if (!u) {
		return NULL;
	}
	(void)snprintf(u, len, "%s/%s", base, suffix);
	return u;
}

static int sync_one(const ftr_repo_cfg *r)
{
	char *dst_dir = NULL;
	char *idx_tmp = NULL;        /* index.toml staging path */
	char *sig_tmp = NULL;        /* index.toml.sig staging path */
	char *idx_final = NULL;
	char *sig_final = NULL;
	char *idx_url = NULL;
	char *sig_url = NULL;
	char err[512];
	ftr_signature sig;
	ftr_pubkey pk;
	int skipped_sig = 0;
	int rc = -1;
	int have_old_index;

	memset(&sig, 0, sizeof(sig));
	memset(&pk, 0, sizeof(pk));

	dst_dir = path_join(3, ftr_db_root(), "sync", r->name);
	if (!dst_dir) {
		err_log("sync: out of memory");
		goto out;
	}
	if (mkdir_p(dst_dir, 0755) != 0) {
		err_log("sync: cannot create '%s'", dst_dir);
		goto out;
	}
	idx_final = path_join(2, dst_dir, "index.toml");
	sig_final = path_join(2, dst_dir, "index.toml.sig");
	if (!idx_final || !sig_final) {
		err_log("sync: out of memory");
		goto out;
	}
	have_old_index = path_exists(idx_final);

	/* Stage downloads next to the final paths so a half-fetched
	 * index.toml never clobbers the previous good one. */
	{
		size_t n = strlen(idx_final) + strlen(".new") + 1;
		idx_tmp = malloc(n);
		if (!idx_tmp) {
			err_log("sync: out of memory");
			goto out;
		}
		(void)snprintf(idx_tmp, n, "%s.new", idx_final);
		n = strlen(sig_final) + strlen(".new") + 1;
		sig_tmp = malloc(n);
		if (!sig_tmp) {
			err_log("sync: out of memory");
			goto out;
		}
		(void)snprintf(sig_tmp, n, "%s.new", sig_final);
	}

	idx_url = join_url(r->url, "index.toml");
	sig_url = join_url(r->url, "index.toml.sig");
	if (!idx_url || !sig_url) {
		err_log("sync: out of memory");
		goto out;
	}

	/* Step 1: pull index. Hard failure if missing. */
	if (ftr_repo_fetch(idx_url, idx_tmp, 0, NULL,
	                   err, sizeof(err)) != 0) {
		err_log("sync: %s", err);
		goto out;
	}

	/* GPG path: when the repo declares a gpgkey, the index is trusted
	 * via a detached GPG signature (index.toml.asc) verified with
	 * gpg(1) — not minisign. */
	if (r->gpgkey && *r->gpgkey) {
		char *asc_url = join_url(r->url, "index.toml.asc");
		char *asc_final = path_join(2, dst_dir, "index.toml.asc");
		char *asc_tmp = NULL;
		if (asc_final) {
			size_t n = strlen(asc_final) + strlen(".new") + 1;
			asc_tmp = malloc(n);
			if (asc_tmp) {
				(void)snprintf(asc_tmp, n, "%s.new", asc_final);
			}
		}
		if (!asc_url || !asc_final || !asc_tmp) {
			err_log("sync: out of memory");
			(void)unlink(idx_tmp);
			free(asc_url); free(asc_final); free(asc_tmp);
			goto out;
		}
		if (ftr_repo_fetch(asc_url, asc_tmp, 0, NULL,
		                   err, sizeof(err)) != 0) {
			err_log("sync: cannot fetch GPG signature: %s", err);
			(void)unlink(idx_tmp);
			free(asc_url); free(asc_final); free(asc_tmp);
			goto out;
		}
		if (ftr_verify_gpg(idx_tmp, asc_tmp, r->gpgkey,
		                   err, sizeof(err)) != 0) {
			fprintf(stderr,
			        "WARN: GPG verification failed for %s; "
			        "keeping previous index (%s)\n", r->name, err);
			(void)unlink(idx_tmp);
			(void)unlink(asc_tmp);
			rc = have_old_index ? 0 : -1;
			free(asc_url); free(asc_final); free(asc_tmp);
			goto out;
		}
		if (rename(idx_tmp, idx_final) != 0 ||
		    rename(asc_tmp, asc_final) != 0) {
			err_log("sync: rename into place failed: %s",
			        strerror(errno));
			free(asc_url); free(asc_final); free(asc_tmp);
			goto out;
		}
		printf("synced: %s (gpg-verified)\n", r->name);
		rc = 0;
		free(asc_url); free(asc_final); free(asc_tmp);
		goto out;
	}

	/* Step 2: pull sig. */
	if (ftr_repo_fetch(sig_url, sig_tmp, 0, &skipped_sig,
	                   err, sizeof(err)) != 0) {
		err_log("sync: %s", err);
		(void)unlink(idx_tmp);
		goto out;
	}

	/* Step 3: load sig, then resolve the trusting key (pin > $FTR_PUBKEY >
	 * keyring by the sig's key_id > built-in default) + verify. A repo whose
	 * signing key is in the keyring needs no per-repo `pubkey` pin. */
	if (ftr_verify_load_signature(sig_tmp, &sig,
	                              err, sizeof(err)) != 0) {
		fprintf(stderr,
		        "WARN: sig verification failed for %s; "
		        "keeping previous index (%s)\n",
		        r->name, err);
		(void)unlink(idx_tmp);
		(void)unlink(sig_tmp);
		rc = have_old_index ? 0 : -1;
		goto out;
	}
	if (ftr_keyring_resolve(r->pubkey, &sig, &pk,
	                        err, sizeof(err)) != 0) {
		err_log("sync: cannot load pubkey for '%s': %s",
		        r->name, err);
		(void)unlink(idx_tmp);
		(void)unlink(sig_tmp);
		goto out;
	}
	if (ftr_verify_archive(idx_tmp, &sig, &pk,
	                       err, sizeof(err)) != 0) {
		fprintf(stderr,
		        "WARN: sig verification failed for %s; "
		        "keeping previous index (%s)\n",
		        r->name, err);
		(void)unlink(idx_tmp);
		(void)unlink(sig_tmp);
		rc = have_old_index ? 0 : -1;
		goto out;
	}

	/* Step 4: rename into place. */
	if (rename(idx_tmp, idx_final) != 0) {
		err_log("sync: rename '%s' -> '%s': %s",
		        idx_tmp, idx_final, strerror(errno));
		goto out;
	}
	if (rename(sig_tmp, sig_final) != 0) {
		err_log("sync: rename '%s' -> '%s': %s",
		        sig_tmp, sig_final, strerror(errno));
		goto out;
	}

	{
		char fp[17];
		ftr_pubkey_fingerprint(&pk, fp);
		printf("synced: %s (verified by %s)\n", r->name, fp);
	}
	rc = 0;

out:
	free(dst_dir);
	free(idx_tmp);
	free(sig_tmp);
	free(idx_final);
	free(sig_final);
	free(idx_url);
	free(sig_url);
	return rc;
}

int cmd_sync(int argc, char **argv)
{
	ftr_repo_list cfg;
	char err[256];
	size_t i;
	int rc = 0;

	memset(&cfg, 0, sizeof(cfg));

	if (argc > 1) {
		const char *a = argv[1];
		if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
			print_help();
			return 0;
		}
		err_log("sync: takes no arguments");
		return 2;
	}

	if (ftr_repo_load_config(&cfg, err, sizeof(err)) != 0) {
		err_log("sync: %s", err);
		return 1;
	}
	if (cfg.n == 0) {
		info_log("sync: no repositories configured in %s",
		         ftr_config_path());
		return 0;
	}

	for (i = 0; i < cfg.n; i++) {
		if (sync_one(&cfg.items[i]) != 0) {
			rc = 1;
			/* keep going — best-effort */
		}
	}

	ftr_repo_list_free(&cfg);
	return rc;
}
