/*
 * keyring.h — a pacman-key-style local trust store for ftr.
 *
 * Today trust is ad-hoc: each repo in feather.conf pins a single
 * `pubkey` file, or the built-in FTR_DEFAULT_PUBKEY is used. That
 * doesn't scale — rotating a key means re-imaging every device.
 *
 * The keyring is a directory of trusted minisign public keys, one file
 * per key named by its 16-hex fingerprint (`<FPR>.pub`). A key being
 * present in the keyring == it is trusted. Verification (sync + install)
 * consults the keyring: a signature's embedded key_id is looked up, and
 * if a trusted key with that id is present the artifact verifies against
 * it — so a repo whose signing key is in the keyring needs NO per-repo
 * `pubkey` pin, and a key can be added/revoked without touching configs.
 *
 *   keyring dir: $FTR_KEYRING, else $FTR_DB_ROOT/keyring
 *
 * Trust resolution order (ftr_keyring_resolve): explicit per-repo pin >
 * $FTR_PUBKEY env > keyring (by signature key_id) > built-in default.
 */
#ifndef FTR_KEYRING_H
#define FTR_KEYRING_H

#include "verify.h"

#include <stddef.h>
#include <stdint.h>

/* Absolute path to the keyring dir ($FTR_KEYRING or <db_root>/keyring).
 * Returns a pointer to a static buffer (not thread-safe; matches
 * ftr_db_root()). */
const char *ftr_keyring_dir(void);

/* Ensure the keyring directory exists (mkdir -p). 0/-1 (errbuf set). */
int ftr_keyring_init(char *errbuf, size_t errbufsz);

/* Import a minisign pubkey file into the keyring as <FPR>.pub. On success
 * the 16-hex fingerprint is written to fpr_out (caller-owned, >=17 bytes)
 * when non-NULL. 0/-1 (errbuf set). */
int ftr_keyring_add(const char *pubkey_file, char fpr_out[17],
                    char *errbuf, size_t errbufsz);

/* Revoke (delete) a trusted key by fingerprint (16 hex chars, case-
 * insensitive). 0 on success, -1 if absent / on error (errbuf set). */
int ftr_keyring_remove(const char *fpr, char *errbuf, size_t errbufsz);

/* Find a trusted key whose key_id matches `key_id`. 0 if found (out
 * filled), -1 if no trusted key matches. */
int ftr_keyring_find(const uint8_t key_id[FTR_MINISIGN_KEYID_BYTES],
                     ftr_pubkey *out);

/* Resolve the pubkey to verify a signature against, honoring the trust
 * order above. `pin_path` is the per-repo pinned pubkey (may be NULL);
 * `sig` supplies the key_id for the keyring lookup (may be NULL to skip
 * it). 0 on success (out filled), -1 otherwise (errbuf set). */
int ftr_keyring_resolve(const char *pin_path, const ftr_signature *sig,
                        ftr_pubkey *out, char *errbuf, size_t errbufsz);

/* Iterate trusted keys. cb receives the fingerprint, parsed key, and the
 * file's untrusted-comment line (may be ""). Returns the key count, or
 * -1 on error. */
typedef void (*ftr_keyring_cb)(const char *fpr, const ftr_pubkey *pk,
                               const char *comment, void *user);
int ftr_keyring_list(ftr_keyring_cb cb, void *user);

#endif /* FTR_KEYRING_H */
