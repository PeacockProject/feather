/*
 * cmd_info.c — `ftr info <pkg>`
 *
 * Phase 4 will print manifest metadata for a locally installed
 * package; phase 6 extends to repo packages.
 */

#include "common.h"

#include <stdio.h>

int cmd_info(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	fprintf(stderr, "ftr info: not implemented yet\n");
	return 0;
}
