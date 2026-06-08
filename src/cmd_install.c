/*
 * cmd_install.c — `ftr install [<options>] <pkg>...`
 *
 * Phase 4: each argument must be a local .feather archive. Package
 * *names* (the phase-6 case — fetch + verify + install from a repo)
 * exit non-zero with a "not yet supported (phase 6)" message.
 *
 * Options:
 *   --peacock-prefix <path>   override /peacock
 *   --apps-prefix <path>      override /apps   (reserved; phase 5)
 *   --compat-prefix <path>    override /compat (reserved; phase 7)
 *   --data-prefix <path>      override /data   (reserved; phase 5)
 *   --help                    one-liner usage
 *
 * On any error, abort with the diagnostic; partial installs are NOT
 * rolled back (cleanup via `ftr remove`).
 */

#include "common.h"
#include "install.h"

#include <stdio.h>
#include <string.h>

static void print_help(void)
{
	printf("Usage: ftr install [options] <pkg.feather>...\n");
	printf("\n");
	printf("Install one or more local .feather archives.\n");
	printf("Repository-based install lands in phase 6.\n");
	printf("\n");
	printf("Options:\n");
	printf("  --peacock-prefix <path>   override /peacock prefix\n");
	printf("  --apps-prefix <path>      override /apps prefix (phase 5)\n");
	printf("  --compat-prefix <path>    override /compat prefix (phase 7)\n");
	printf("  --data-prefix <path>      override /data prefix (phase 5)\n");
	printf("  -h, --help                show this help\n");
	printf("\n");
	printf("Note: partial-install rollback is not implemented in "
	       "phase 4 — if an install fails mid-way, run `ftr remove "
	       "<pkg>` to clean up.\n");
}

/* Does `arg` end in ".feather"? Cheap heuristic that tells "local
 * archive" from "repo package name" on the CLI. */
static int looks_like_archive(const char *arg)
{
	size_t n = strlen(arg);
	return n >= 8 && strcmp(arg + n - 8, ".feather") == 0;
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
		if (!looks_like_archive(arg)) {
			err_log("install: '%s': repo-based install not yet "
			        "supported (phase 6)", arg);
			return 1;
		}
		if (ftr_install_local(arg, &opts) != 0) {
			return 1;
		}
	}

	if (targets == 0) {
		print_help();
		return 1;
	}
	return 0;
}
