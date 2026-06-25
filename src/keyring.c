/*
 * keyring.c — local trust store (see keyring.h).
 */
#include "keyring.h"
#include "db.h"
#include "util.h"
#include "verify.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Uppercase a 16-hex fingerprint in place (minisign prints upper-hex). */
static void fpr_upper(char *s)
{
	for (; *s; s++) {
		*s = (char)toupper((unsigned char)*s);
	}
}

/* True iff `name` is "<16 hex>.pub". Copies the bare fingerprint (no
 * extension) into fpr_out (>=17) when non-NULL. */
static int is_keyfile(const char *name, char *fpr_out)
{
	size_t len = strlen(name);
	size_t i;
	if (len != 16 + 4)
		return 0;
	if (strcmp(name + 16, ".pub") != 0)
		return 0;
	for (i = 0; i < 16; i++) {
		if (!isxdigit((unsigned char)name[i]))
			return 0;
	}
	if (fpr_out) {
		memcpy(fpr_out, name, 16);
		fpr_out[16] = '\0';
		fpr_upper(fpr_out);
	}
	return 1;
}

const char *ftr_keyring_dir(void)
{
	static char cached[4096];
	const char *env = getenv("FTR_KEYRING");
	if (env && *env) {
		(void)snprintf(cached, sizeof(cached), "%s", env);
	} else {
		(void)snprintf(cached, sizeof(cached), "%s/keyring",
		               ftr_db_root());
	}
	cached[sizeof(cached) - 1] = '\0';
	return cached;
}

int ftr_keyring_init(char *errbuf, size_t errbufsz)
{
	const char *dir = ftr_keyring_dir();
	if (mkdir_p(dir, 0755) != 0) {
		if (errbuf)
			(void)snprintf(errbuf, errbufsz,
			               "cannot create keyring dir '%s'", dir);
		return -1;
	}
	return 0;
}

int ftr_keyring_add(const char *pubkey_file, char fpr_out[17],
                    char *errbuf, size_t errbufsz)
{
	ftr_pubkey pk;
	char fpr[17];
	char *dest;

	if (ftr_verify_load_pubkey(pubkey_file, &pk, errbuf, errbufsz) != 0)
		return -1;
	if (ftr_keyring_init(errbuf, errbufsz) != 0)
		return -1;

	ftr_pubkey_fingerprint(&pk, fpr);
	dest = path_join(2, ftr_keyring_dir(), fpr);
	if (!dest) {
		if (errbuf)
			(void)snprintf(errbuf, errbufsz, "out of memory");
		return -1;
	}
	/* keyring files are "<FPR>.pub" */
	{
		size_t n = strlen(dest);
		char *withext = malloc(n + 5);
		if (!withext) {
			free(dest);
			if (errbuf)
				(void)snprintf(errbuf, errbufsz, "out of memory");
			return -1;
		}
		memcpy(withext, dest, n);
		memcpy(withext + n, ".pub", 5);
		free(dest);
		dest = withext;
	}

	if (copy_file(pubkey_file, dest) != 0) {
		if (errbuf)
			(void)snprintf(errbuf, errbufsz,
			               "cannot write key to '%s'", dest);
		free(dest);
		return -1;
	}
	free(dest);
	if (fpr_out) {
		memcpy(fpr_out, fpr, 17);
	}
	return 0;
}

int ftr_keyring_remove(const char *fpr, char *errbuf, size_t errbufsz)
{
	char norm[17];
	char *path;
	size_t n;

	if (!fpr || strlen(fpr) != 16) {
		if (errbuf)
			(void)snprintf(errbuf, errbufsz,
			               "fingerprint must be 16 hex chars");
		return -1;
	}
	memcpy(norm, fpr, 16);
	norm[16] = '\0';
	fpr_upper(norm);

	path = path_join(2, ftr_keyring_dir(), norm);
	if (!path) {
		if (errbuf)
			(void)snprintf(errbuf, errbufsz, "out of memory");
		return -1;
	}
	n = strlen(path);
	{
		char *withext = malloc(n + 5);
		if (!withext) {
			free(path);
			if (errbuf)
				(void)snprintf(errbuf, errbufsz, "out of memory");
			return -1;
		}
		memcpy(withext, path, n);
		memcpy(withext + n, ".pub", 5);
		free(path);
		path = withext;
	}

	if (unlink(path) != 0) {
		if (errbuf)
			(void)snprintf(errbuf, errbufsz,
			               "no trusted key '%s' in keyring", norm);
		free(path);
		return -1;
	}
	free(path);
	return 0;
}

/* Read the first line (the "untrusted comment: ..." header) of a pubkey
 * file into out (>=1). Best-effort; out is "" on failure. */
static void read_comment(const char *path, char *out, size_t outsz)
{
	FILE *f;
	out[0] = '\0';
	f = fopen(path, "r");
	if (!f)
		return;
	if (fgets(out, (int)outsz, f)) {
		size_t l = strlen(out);
		while (l > 0 && (out[l - 1] == '\n' || out[l - 1] == '\r'))
			out[--l] = '\0';
	}
	fclose(f);
}

int ftr_keyring_find(const uint8_t key_id[FTR_MINISIGN_KEYID_BYTES],
                     ftr_pubkey *out)
{
	const char *dir = ftr_keyring_dir();
	DIR *d = opendir(dir);
	struct dirent *de;
	int found = -1;
	if (!d)
		return -1;
	while ((de = readdir(d)) != NULL) {
		char fpr[17];
		char *path;
		ftr_pubkey pk;
		char errbuf[256];
		if (!is_keyfile(de->d_name, fpr))
			continue;
		path = path_join(2, dir, de->d_name);
		if (!path)
			continue;
		if (ftr_verify_load_pubkey(path, &pk, errbuf,
		                           sizeof(errbuf)) == 0) {
			if (memcmp(pk.key_id, key_id,
			           FTR_MINISIGN_KEYID_BYTES) == 0) {
				if (out)
					*out = pk;
				found = 0;
				free(path);
				break;
			}
		}
		free(path);
	}
	closedir(d);
	return found;
}

int ftr_keyring_resolve(const char *pin_path, const ftr_signature *sig,
                        ftr_pubkey *out, char *errbuf, size_t errbufsz)
{
	const char *env_path;

	/* 1. explicit per-repo pin always wins (backward compatible). */
	if (pin_path && *pin_path)
		return ftr_verify_load_pubkey(pin_path, out, errbuf, errbufsz);

	/* 2. $FTR_PUBKEY test/override. */
	env_path = getenv("FTR_PUBKEY");
	if (env_path && *env_path)
		return ftr_verify_load_pubkey(env_path, out, errbuf, errbufsz);

	/* 3. the keyring, matched by the signature's key_id. */
	if (sig && ftr_keyring_find(sig->key_id, out) == 0)
		return 0;

	/* 4. built-in default (the shipped placeholder/root key). */
	return ftr_verify_parse_pubkey(FTR_DEFAULT_PUBKEY, out, errbuf,
	                               errbufsz);
}

int ftr_keyring_list(ftr_keyring_cb cb, void *user)
{
	const char *dir = ftr_keyring_dir();
	DIR *d = opendir(dir);
	struct dirent *de;
	int count = 0;
	if (!d)
		return 0; /* no keyring yet == zero trusted keys */
	while ((de = readdir(d)) != NULL) {
		char fpr[17];
		char *path;
		ftr_pubkey pk;
		char comment[1024];
		char errbuf[256];
		if (!is_keyfile(de->d_name, fpr))
			continue;
		path = path_join(2, dir, de->d_name);
		if (!path)
			continue;
		if (ftr_verify_load_pubkey(path, &pk, errbuf,
		                           sizeof(errbuf)) == 0) {
			read_comment(path, comment, sizeof(comment));
			if (cb)
				cb(fpr, &pk, comment, user);
			count++;
		}
		free(path);
	}
	closedir(d);
	return count;
}
