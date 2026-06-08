/*
 * cmd_upgrade.c — `ftr upgrade [<pkg>...]`
 *
 * Phase 6 will pull newer signed builds from the repo.
 */

#include "common.h"

#include <stdio.h>

int cmd_upgrade(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	fprintf(stderr, "ftr upgrade: not implemented yet\n");
	return 0;
}
