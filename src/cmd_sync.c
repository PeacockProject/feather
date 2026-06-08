/*
 * cmd_sync.c — `ftr sync`
 *
 * Read /etc/feather/feather.conf (or $FTR_CONFIG) and for each
 * [[repos]] entry fetch <url>/index.toml into
 * $FTR_DB_ROOT/sync/<repo-name>/index.toml.
 *
 * Phase 4b: only file:// URLs are honoured; other schemes log a
 * one-line skip and don't fail the whole sync. Idempotent: re-running
 * just overwrites the local index.
 */

#define _POSIX_C_SOURCE 200809L

#include "common.h"
#include "db.h"
#include "repo.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_help(void)
{
	printf("Usage: ftr sync\n");
	printf("\n");
	printf("Refresh repository metadata. Reads %s\n", FTR_CONFIG);
	printf("(or $FTR_CONFIG) and copies each repo's index.toml into\n");
	printf("$FTR_DB_ROOT/sync/<repo-name>/index.toml.\n");
	printf("\n");
	printf("Phase 4b supports file:// URLs only; other schemes are\n");
	printf("skipped with a phase-6 note.\n");
}

static int sync_one(const ftr_repo_cfg *r)
{
	char *dst_dir = NULL;
	char *dst_path = NULL;
	char *src_url = NULL;
	size_t url_len;
	int skipped = 0;
	int rc = -1;

	dst_dir = path_join(3, ftr_db_root(), "sync", r->name);
	if (!dst_dir) {
		err_log("sync: out of memory");
		goto out;
	}
	if (mkdir_p(dst_dir, 0755) != 0) {
		err_log("sync: cannot create '%s'", dst_dir);
		goto out;
	}
	dst_path = path_join(2, dst_dir, "index.toml");
	if (!dst_path) {
		err_log("sync: out of memory");
		goto out;
	}

	/* Build "<url>/index.toml". For file:// strip+rejoin so paths
	 * stay clean; for other schemes we still hand the unmolested URL
	 * to ftr_repo_fetch which will skip-and-log. */
	url_len = strlen(r->url) + strlen("/index.toml") + 1;
	src_url = malloc(url_len);
	if (!src_url) {
		err_log("sync: out of memory");
		goto out;
	}
	(void)snprintf(src_url, url_len, "%s/index.toml", r->url);

	if (ftr_repo_fetch(src_url, dst_path, &skipped) != 0) {
		err_log("sync: failed to fetch '%s'", src_url);
		goto out;
	}
	if (skipped) {
		/* Non-file:// scheme — repo_fetch already logged a note. */
		rc = 0;
		goto out;
	}

	printf("synced: %s\n", r->name);
	rc = 0;

out:
	free(dst_dir);
	free(dst_path);
	free(src_url);
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
