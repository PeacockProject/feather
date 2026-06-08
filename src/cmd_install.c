/*
 * cmd_install.c — `ftr install <pkg>...`
 *
 * Phase 4 will implement local-archive overlay install for
 * `layout = "peacock"`. Phase 6 adds repo fetch + signature verify.
 */

#include "common.h"

#include <stdio.h>

int cmd_install(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	fprintf(stderr, "ftr install: not implemented yet\n");
	return 0;
}
