/*
 * cmd_remove.c — `ftr remove <pkg-name>...`
 *
 * For each package name on the command line:
 *
 *   1. resolve <name> -> installed version via ftr_db_find_version
 *      (phase 4: at most one version installed per name)
 *   2. run hooks/pre-remove.sh if recorded in the DB (env vars
 *      mirror cmd_install: FEATHER_PREFIX / FEATHER_PKG_NAME /
 *      FEATHER_PKG_VERSION; FEATHER_PREFIX is the leading component
 *      of the first recorded file for context)
 *   3. unlink every file recorded in the DB's `files` list (paths
 *      are stored without a leading slash so we re-prepend "/").
 *      Best-effort rmdir() on parent dirs to prune empties.
 *   4. run hooks/post-remove.sh
 *   5. ftr_db_remove() — drop the DB entry
 *
 * Errors short-circuit. Missing-on-disk files are tolerated (ENOENT
 * during unlink is silently swallowed) so a re-run of `ftr remove`
 * after a partial-failure first run completes cleanly.
 */

#define _POSIX_C_SOURCE 200809L

#include "common.h"
#include "db.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void print_help(void)
{
	printf("Usage: ftr remove <pkg-name>...\n");
	printf("\n");
	printf("Remove one or more installed packages.\n");
	printf("\n");
	printf("Options:\n");
	printf("  -h, --help   show this help\n");
}

/* Spawn `sh <script>` with FEATHER_* env vars set. Returns 0 on
 * success (incl. script absent). */
static int run_hook_file(const char *script,
                         const char *prefix_for_env,
                         const char *name, const char *version)
{
	pid_t pid;
	int status;
	if (!script) {
		return 0;
	}
	pid = fork();
	if (pid < 0) {
		return -1;
	}
	if (pid == 0) {
		setenv("FEATHER_PREFIX", prefix_for_env ? prefix_for_env : "",
		       1);
		setenv("FEATHER_PKG_NAME", name ? name : "", 1);
		setenv("FEATHER_PKG_VERSION", version ? version : "", 1);
		execlp("sh", "sh", script, (char *)NULL);
		_exit(127);
	}
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR) {
			return -1;
		}
	}
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		return 0;
	}
	return -1;
}

/* Try rmdir() on each parent of `path` up the tree. Stops at the
 * first non-empty parent (ENOTEMPTY) or the root. Best effort — any
 * other failure is silently ignored. */
static void prune_parents(const char *path)
{
	char *dup = strdup(path);
	char *slash;
	if (!dup) {
		return;
	}
	for (;;) {
		slash = strrchr(dup, '/');
		if (!slash || slash == dup) {
			break;
		}
		*slash = '\0';
		if (rmdir(dup) != 0) {
			break;
		}
	}
	free(dup);
}

/* Remove a single installed package by name. */
static int remove_one(const char *name)
{
	char *version = NULL;
	ftr_db_entry entry;
	char *pre_hook = NULL;
	char *post_hook = NULL;
	char *guess_prefix = NULL;
	size_t i;
	int rc = -1;

	memset(&entry, 0, sizeof(entry));

	version = ftr_db_find_version(name);
	if (!version) {
		err_log("remove: package '%s' is not installed", name);
		return -1;
	}
	if (ftr_db_get(name, version, &entry) != 0) {
		err_log("remove: cannot load DB entry for %s-%s",
		        name, version);
		free(version);
		return -1;
	}

	/* Heuristic: FEATHER_PREFIX for hooks is the leading "/<dir>"
	 * of the first recorded file, e.g. "/peacock" for paths like
	 * "peacock/bin/foo". Used for env, not for any real path
	 * arithmetic. */
	if (entry.n_files > 0) {
		const char *p = entry.files[0];
		const char *slash = strchr(p, '/');
		size_t n = slash ? (size_t)(slash - p) : strlen(p);
		guess_prefix = malloc(n + 2);
		if (guess_prefix) {
			guess_prefix[0] = '/';
			memcpy(guess_prefix + 1, p, n);
			guess_prefix[n + 1] = '\0';
		}
	}

	pre_hook = ftr_db_hook_path(name, version, "pre-remove");
	if (pre_hook) {
		if (run_hook_file(pre_hook, guess_prefix, name, version) != 0) {
			err_log("remove: pre-remove hook failed for %s-%s",
			        name, version);
			goto out;
		}
	}

	/* Unlink each recorded file, deepest-first. The DB stores the
	 * list sorted lexicographically; reverse-iteration approximates
	 * deepest-first well enough for prune_parents() to clean up
	 * empty dirs in the common case. */
	for (i = entry.n_files; i > 0; i--) {
		const char *rel = entry.files[i - 1];
		char *abs = path_join(2, "/", rel);
		struct stat st;

		if (!abs) {
			err_log("remove: out of memory");
			goto out;
		}
		if (lstat(abs, &st) != 0) {
			if (errno == ENOENT) {
				free(abs);
				continue;
			}
			err_log("remove: cannot stat '%s': %s",
			        abs, strerror(errno));
			free(abs);
			goto out;
		}
		if (S_ISDIR(st.st_mode)) {
			/* Skip dirs in the recorded list; prune_parents
			 * will clean them up after their children go. */
			free(abs);
			continue;
		}
		if (unlink(abs) != 0) {
			err_log("remove: cannot unlink '%s': %s",
			        abs, strerror(errno));
			free(abs);
			goto out;
		}
		prune_parents(abs);
		free(abs);
	}

	post_hook = ftr_db_hook_path(name, version, "post-remove");
	if (post_hook) {
		if (run_hook_file(post_hook, guess_prefix, name, version) != 0) {
			err_log("remove: post-remove hook failed for %s-%s",
			        name, version);
			goto out;
		}
	}

	if (ftr_db_remove(name, version) != 0) {
		err_log("remove: cannot drop DB entry for %s-%s",
		        name, version);
		goto out;
	}

	printf("removed: %s-%s\n", name, version);
	rc = 0;

out:
	ftr_db_entry_free(&entry);
	free(version);
	free(pre_hook);
	free(post_hook);
	free(guess_prefix);
	return rc;
}

int cmd_remove(int argc, char **argv)
{
	int i;
	int targets = 0;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
			print_help();
			return 0;
		}
		if (a[0] == '-' && a[1] != '\0') {
			err_log("remove: unknown option '%s'", a);
			return 2;
		}
		break;
	}

	for (; i < argc; i++) {
		targets++;
		if (remove_one(argv[i]) != 0) {
			return 1;
		}
	}

	if (targets == 0) {
		print_help();
		return 1;
	}
	return 0;
}
