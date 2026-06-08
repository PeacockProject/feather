/*
 * cmd_list.c — `ftr list`
 *
 * Walk the local DB and print one "<name> <version>" line per
 * installed package, sorted lexicographically (the DB hands them back
 * already sorted).
 */

#include "common.h"
#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_help(void)
{
	printf("Usage: ftr list\n");
	printf("\n");
	printf("List every installed package as \"<name> <version>\".\n");
}

int cmd_list(int argc, char **argv)
{
	char **names = NULL;
	size_t n = 0;
	size_t i;

	if (argc > 1) {
		const char *a = argv[1];
		if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
			print_help();
			return 0;
		}
		err_log("list: takes no arguments");
		return 2;
	}

	if (ftr_db_list(&names, &n) != 0) {
		err_log("list: cannot read DB");
		return 1;
	}
	for (i = 0; i < n; i++) {
		/* DB hands us "<name>-<version>"; split at the last '-'
		 * for display. */
		const char *combo = names[i];
		const char *dash = strrchr(combo, '-');
		if (dash && dash != combo) {
			printf("%.*s %s\n",
			       (int)(dash - combo), combo,
			       dash + 1);
		} else {
			printf("%s\n", combo);
		}
	}
	for (i = 0; i < n; i++) {
		free(names[i]);
	}
	free(names);
	return 0;
}
