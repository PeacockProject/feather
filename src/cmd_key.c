/*
 * cmd_key.c — `ftr key` — manage the local trust keyring.
 *
 *   ftr key list                 list trusted keys
 *   ftr key add <file>...        import + trust minisign pubkey file(s)
 *   ftr key remove <fpr>...      revoke trusted key(s) by fingerprint
 *   ftr key fingerprint <file>.. print a pubkey file's fingerprint (no change)
 *   ftr key dir                  print the keyring directory
 *   ftr key init                 create the keyring directory
 *
 * See keyring.h for the trust model.
 */
#include "common.h"
#include "keyring.h"
#include "verify.h"

#include <stdio.h>
#include <string.h>

static int key_usage(void)
{
	fprintf(stderr,
	    "usage: ftr key <subcommand>\n"
	    "  list                  list trusted keys\n"
	    "  add <file>...         import + trust minisign pubkey file(s)\n"
	    "  remove <fpr>...       revoke trusted key(s) by fingerprint\n"
	    "  fingerprint <file>... show a pubkey file's fingerprint\n"
	    "  dir                   print the keyring directory\n"
	    "  init                  create the keyring directory\n");
	return 2;
}

static void list_cb(const char *fpr, const ftr_pubkey *pk,
                    const char *comment, void *user)
{
	int *n = (int *)user;
	(void)pk;
	(*n)++;
	if (comment && *comment)
		printf("%s  %s\n", fpr, comment);
	else
		printf("%s\n", fpr);
}

static int key_list(void)
{
	int n = 0;
	int total = ftr_keyring_list(list_cb, &n);
	if (total <= 0) {
		printf("keyring empty (%s)\n", ftr_keyring_dir());
		return 0;
	}
	printf("%d trusted key(s) in %s\n", total, ftr_keyring_dir());
	return 0;
}

static int key_add(int argc, char **argv)
{
	int i;
	int failures = 0;
	if (argc < 1)
		return key_usage();
	for (i = 0; i < argc; i++) {
		char fpr[17];
		char err[256];
		if (ftr_keyring_add(argv[i], fpr, err, sizeof(err)) != 0) {
			err_log("key add: %s: %s", argv[i], err);
			failures++;
			continue;
		}
		printf("trusted: %s (%s)\n", fpr, argv[i]);
	}
	return failures ? 1 : 0;
}

static int key_remove(int argc, char **argv)
{
	int i;
	int failures = 0;
	if (argc < 1)
		return key_usage();
	for (i = 0; i < argc; i++) {
		char err[256];
		if (ftr_keyring_remove(argv[i], err, sizeof(err)) != 0) {
			err_log("key remove: %s", err);
			failures++;
			continue;
		}
		printf("revoked: %s\n", argv[i]);
	}
	return failures ? 1 : 0;
}

static int key_fingerprint(int argc, char **argv)
{
	int i;
	int failures = 0;
	if (argc < 1)
		return key_usage();
	for (i = 0; i < argc; i++) {
		ftr_pubkey pk;
		char fpr[17];
		char err[256];
		if (ftr_verify_load_pubkey(argv[i], &pk, err,
		                           sizeof(err)) != 0) {
			err_log("key fingerprint: %s: %s", argv[i], err);
			failures++;
			continue;
		}
		ftr_pubkey_fingerprint(&pk, fpr);
		printf("%s  %s\n", fpr, argv[i]);
	}
	return failures ? 1 : 0;
}

int cmd_key(int argc, char **argv)
{
	const char *sub;
	if (argc < 2)
		return key_usage();
	sub = argv[1];

	if (strcmp(sub, "list") == 0 || strcmp(sub, "ls") == 0)
		return key_list();
	if (strcmp(sub, "add") == 0 || strcmp(sub, "import") == 0)
		return key_add(argc - 2, argv + 2);
	if (strcmp(sub, "remove") == 0 || strcmp(sub, "del") == 0 ||
	    strcmp(sub, "revoke") == 0)
		return key_remove(argc - 2, argv + 2);
	if (strcmp(sub, "fingerprint") == 0 || strcmp(sub, "fpr") == 0)
		return key_fingerprint(argc - 2, argv + 2);
	if (strcmp(sub, "dir") == 0) {
		printf("%s\n", ftr_keyring_dir());
		return 0;
	}
	if (strcmp(sub, "init") == 0) {
		char err[256];
		if (ftr_keyring_init(err, sizeof(err)) != 0) {
			err_log("key init: %s", err);
			return 1;
		}
		printf("keyring ready: %s\n", ftr_keyring_dir());
		return 0;
	}
	return key_usage();
}
