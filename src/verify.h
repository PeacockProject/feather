/*
 * verify.h — minisign-format Ed25519 signature verification.
 *
 * Phase 6: every package shipped by the feather build farm is signed
 * with the minisign tool. `ftr` ships a compile-time root public key
 * (overridable via $FTR_PUBKEY for testing) and refuses to install
 * repo packages whose signature doesn't verify. Local installs from a
 * filesystem path may carry a sidecar `.sig`; if absent, install is
 * permitted with a WARN line. `--allow-unsigned` skips verification
 * for local installs only.
 *
 * Format reference: https://jedisct1.github.io/minisign/
 *
 *   pubkey file:
 *     untrusted comment: minisign public key XXXXXXXX
 *     <base64 of [alg_tag(2) | key_id(8) | ed25519_pk(32)]>
 *
 *   signature file:
 *     untrusted comment: ...
 *     <base64 of [alg_tag(2) | key_id(8) | ed25519_sig(64)]>
 *     trusted comment: ...
 *     <base64 of [ed25519_sig(64)] — sig over (sig||trusted_comment)>
 *
 * Algorithm tag: only "Ed" (0x4564) is accepted. Legacy "ED" (hashed
 * mode) and any other tag are rejected with `unsupported signature
 * algorithm` in errbuf.
 */

#ifndef FTR_VERIFY_H
#define FTR_VERIFY_H

#include <stddef.h>
#include <stdint.h>

/* PHASE 6 PLACEHOLDER KEY. Replace with the production farm key
 * before shipping. The corresponding seckey lives at
 * tests/data/test_seckey.txt and is used only by tests + tools. */
extern const char FTR_DEFAULT_PUBKEY[];

#define FTR_PUBKEY_RAW_BYTES   42   /* 2-byte alg + 8-byte key_id + 32-byte pk */
#define FTR_SIG_RAW_BYTES      74   /* 2-byte alg + 8-byte key_id + 64-byte sig */
#define FTR_GLOBAL_SIG_BYTES   64   /* second signature in a .sig file */
#define FTR_ED25519_PK_BYTES   32
#define FTR_ED25519_SIG_BYTES  64
#define FTR_MINISIGN_KEYID_BYTES 8
#define FTR_MINISIGN_ALG_ED     0x4564  /* little-endian "Ed" */
#define FTR_MINISIGN_ALG_HASHED 0x4445  /* little-endian "ED" — legacy, rejected */

typedef struct {
	uint16_t alg;                                  /* always 0x4564 */
	uint8_t  key_id[FTR_MINISIGN_KEYID_BYTES];
	uint8_t  pk[FTR_ED25519_PK_BYTES];
} ftr_pubkey;

typedef struct {
	uint16_t alg;                                  /* always 0x4564 */
	uint8_t  key_id[FTR_MINISIGN_KEYID_BYTES];
	uint8_t  sig[FTR_ED25519_SIG_BYTES];
	char     trusted_comment[1024];
	size_t   trusted_comment_len;
	uint8_t  global_sig[FTR_GLOBAL_SIG_BYTES];
} ftr_signature;

/* Write a stable 16-hex-char fingerprint of `pk`'s key_id into
 * `out` (caller-owned, >= 17 bytes). */
void ftr_pubkey_fingerprint(const ftr_pubkey *pk, char out[17]);

/* Parse a minisign-format pubkey from a file path. Returns 0 on
 * success, -1 on failure (errbuf populated). */
int ftr_verify_load_pubkey(const char *path, ftr_pubkey *out,
                           char *errbuf, size_t errbufsz);

/* Parse a minisign-format pubkey from a NUL-terminated string. Same
 * contract as the file version. Used for the built-in
 * FTR_DEFAULT_PUBKEY constant. */
int ftr_verify_parse_pubkey(const char *text, ftr_pubkey *out,
                            char *errbuf, size_t errbufsz);

/* Parse a minisign .sig file from `path`. Returns 0 on success. */
int ftr_verify_load_signature(const char *path, ftr_signature *out,
                              char *errbuf, size_t errbufsz);

/* Verify `archive_path` against (`sig`, `pk`). Computes Ed25519
 * verify of the archive bytes, then verifies the global signature
 * over (sig||trusted_comment) so a tampered trusted_comment is
 * rejected. Returns 0 on success, -1 on failure with errbuf
 * populated. */
int ftr_verify_archive(const char *archive_path,
                       const ftr_signature *sig,
                       const ftr_pubkey *pk,
                       char *errbuf, size_t errbufsz);

/* Resolve the effective pubkey for a repo: per-repo file via
 * `pubkey_path` (may be NULL), $FTR_PUBKEY env override, or the
 * built-in FTR_DEFAULT_PUBKEY. Returns 0 on success. */
int ftr_verify_resolve_pubkey(const char *pubkey_path,
                              ftr_pubkey *out,
                              char *errbuf, size_t errbufsz);

#endif /* FTR_VERIFY_H */
