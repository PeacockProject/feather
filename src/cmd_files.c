/*
 * cmd_files.c — `ftr files <pkg-name>`
 *
 * Print the recorded files list for one installed package, one path
 * per line, in sorted order. Paths are absolute (a leading '/' is
 * re-added to the DB's relative form).
 */

#include "common.h"
#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_help(void)
{
	printf("Usage: ftr files <pkg-name>\n");
	printf("\n");
	printf("List every file owned by an installed package.\n");
}

int cmd_files(int argc, char **argv)
{
	const char *name;
	char *version = NULL;
	ftr_db_entry entry;
	size_t i;

	if (argc < 2) {
		print_help();
		return 1;
	}
	if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
		print_help();
		return 0;
	}
	if (argc > 2) {
		err_log("files: takes exactly one package name");
		return 2;
	}

	name = argv[1];
	memset(&entry, 0, sizeof(entry));
	version = ftr_db_find_version(name);
	if (!version) {
		err_log("files: package '%s' is not installed", name);
		return 1;
	}
	if (ftr_db_get(name, version, &entry) != 0) {
		err_log("files: cannot read DB entry for %s-%s",
		        name, version);
		free(version);
		return 1;
	}
	for (i = 0; i < entry.n_files; i++) {
		printf("/%s\n", entry.files[i]);
	}
	ftr_db_entry_free(&entry);
	free(version);
	return 0;
}
