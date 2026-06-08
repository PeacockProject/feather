/*
 * cmd_search.c — `ftr search <pattern>`
 *
 * Case-insensitive substring match against every package's `name` and
 * `description` across every synced repo index. Output:
 *
 *   <repo>/<name> <version> — <description>
 *
 * No-op success (exit 0, empty stdout) when no repos are synced.
 */

#define _POSIX_C_SOURCE 200809L

#include "common.h"
#include "repo.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_help(void)
{
	printf("Usage: ftr search <pattern>\n");
	printf("\n");
	printf("Case-insensitive substring search across synced repo\n");
	printf("indexes. Matches against package name and description.\n");
}

/* Case-insensitive substring search. Returns 1 if `needle` appears in
 * `haystack`, 0 otherwise. Handles NULL haystack as empty. */
static int icontains(const char *haystack, const char *needle)
{
	size_t hl;
	size_t nl;
	size_t i;
	if (!needle || !*needle) {
		return 1;
	}
	if (!haystack) {
		return 0;
	}
	hl = strlen(haystack);
	nl = strlen(needle);
	if (nl > hl) {
		return 0;
	}
	for (i = 0; i + nl <= hl; i++) {
		size_t j;
		int ok = 1;
		for (j = 0; j < nl; j++) {
			int a = (unsigned char)haystack[i + j];
			int b = (unsigned char)needle[j];
			if (tolower(a) != tolower(b)) {
				ok = 0;
				break;
			}
		}
		if (ok) {
			return 1;
		}
	}
	return 0;
}

int cmd_search(int argc, char **argv)
{
	const char *pattern;
	ftr_repo_index *idxs = NULL;
	size_t n_idxs = 0;
	size_t i;
	size_t j;

	if (argc < 2) {
		print_help();
		return 1;
	}
	if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
		print_help();
		return 0;
	}
	if (argc > 2) {
		err_log("search: takes exactly one pattern");
		return 2;
	}
	pattern = argv[1];

	if (ftr_repo_load_all_synced(&idxs, &n_idxs) != 0) {
		err_log("search: cannot read synced repos");
		return 1;
	}
	for (i = 0; i < n_idxs; i++) {
		const ftr_repo_index *idx = &idxs[i];
		for (j = 0; j < idx->n_entries; j++) {
			const ftr_pkg_index_entry *e = &idx->entries[j];
			if (icontains(e->name, pattern) ||
			    icontains(e->description, pattern)) {
				printf("%s/%s %s",
				       idx->repo_name, e->name, e->version);
				if (e->description) {
					printf(" - %s", e->description);
				}
				putchar('\n');
			}
		}
	}
	ftr_repo_indexes_free(idxs, n_idxs);
	return 0;
}
