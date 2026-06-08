/*
 * cmd_files.c — `ftr files <pkg>`
 *
 * Phase 4 will list files owned by an installed package, reading
 * the per-package files manifest from /var/lib/feather/local/.
 */

#include "common.h"

#include <stdio.h>

int cmd_files(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	fprintf(stderr, "ftr files: not implemented yet\n");
	return 0;
}
