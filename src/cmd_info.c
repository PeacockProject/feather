/*
 * cmd_info.c — `ftr info <name>`
 *
 * Find the highest-version entry for <name> across all synced repo
 * indexes (first-listed repo wins on tie) and print its metadata.
 * No-op success when no repos are synced or no match found (consistent
 * with `ftr search`).
 */

#define _POSIX_C_SOURCE 200809L

#include "common.h"
#include "repo.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_help(void)
{
	printf("Usage: ftr info <pkg-name>\n");
	printf("\n");
	printf("Show metadata for the highest-version entry of <pkg-name>\n");
	printf("across every synced repo. Prints nothing on no match.\n");
}

int cmd_info(int argc, char **argv)
{
	const char *name;
	ftr_repo_index *idxs = NULL;
	size_t n_idxs = 0;
	size_t i;
	size_t j;
	const ftr_pkg_index_entry *best = NULL;

	if (argc < 2) {
		print_help();
		return 1;
	}
	if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
		print_help();
		return 0;
	}
	if (argc > 2) {
		err_log("info: takes exactly one package name");
		return 2;
	}
	name = argv[1];

	if (ftr_repo_load_all_synced(&idxs, &n_idxs) != 0) {
		err_log("info: cannot read synced repos");
		return 1;
	}
	for (i = 0; i < n_idxs; i++) {
		const ftr_repo_index *idx = &idxs[i];
		for (j = 0; j < idx->n_entries; j++) {
			const ftr_pkg_index_entry *e = &idx->entries[j];
			if (strcmp(e->name, name) != 0) {
				continue;
			}
			if (!best ||
			    version_compare(e->version, best->version) > 0) {
				best = e;
			}
		}
	}

	if (best) {
		printf("Name:        %s\n", best->name);
		printf("Version:     %s\n", best->version);
		printf("Repository:  %s\n", best->repo_name);
		if (best->description) {
			printf("Description: %s\n", best->description);
		}
		if (best->runtime) {
			printf("Runtime:     %s\n", best->runtime);
		}
		if (best->layout) {
			printf("Layout:      %s\n", best->layout);
		}
		printf("Archive:     %s\n", best->archive);
		if (best->sha256) {
			printf("SHA-256:     %s\n", best->sha256);
		}
		if (best->size >= 0) {
			printf("Size:        %lld bytes\n", best->size);
		}
	}

	ftr_repo_indexes_free(idxs, n_idxs);
	return 0;
}
