/*
 * tools/gen-keypair.c — deterministic minisign keypair generator.
 *
 * Built outside the default `ftr` target; not installed. Tests use
 * this to mint a reproducible keypair without depending on a host
 * `minisign` binary or `/dev/urandom`. Production keys are generated
 * on the build farm with real `minisign`, never with this tool.
 *
 * Usage:
 *
 *   gen-keypair <seed-string> <pubkey-out-path> <seckey-out-path> \
 *               [trusted-comment]
 *
 * The seed is hashed with SHA-512 and the leading 32 bytes feed
 * TweetNaCl's crypto_sign_keypair() via a tool-local override of the
 * `randombytes` symbol. Same seed string ⇒ same pubkey ⇒ same key id.
 *
 * Output files match minisign's text format. The .sec file we emit
 * uses the unencrypted layout (kdf_alg = 0x0000) so test scripts can
 * sign without prompting for a passphrase.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "tweetnacl.h"
#include "verify.h"

/* ---------------------------------------------------------------- *
 * deterministic randombytes (overrides the /dev/urandom stub in
 * src/randombytes.o — this object is NOT linked into gen-keypair).
 * ---------------------------------------------------------------- */

static const uint8_t *g_seed_buf;
static size_t g_seed_len;
static size_t g_seed_off;

void randombytes(unsigned char *buf, unsigned long long n);

void randombytes(unsigned char *buf, unsigned long long n)
{
	unsigned long long i;
	if (!g_seed_buf || g_seed_len == 0) {
		fprintf(stderr, "gen-keypair: randombytes called with no "
		                "seed configured\n");
		exit(2);
	}
	for (i = 0; i < n; i++) {
		buf[i] = g_seed_buf[g_seed_off % g_seed_len];
		g_seed_off++;
	}
}

/* ---------------------------------------------------------------- *
 * tiny base64 encoder (RFC 4648, padded, no newlines)
 * ---------------------------------------------------------------- */

static const char B64[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_encode(const uint8_t *in, size_t n, char *out, size_t outcap)
{
	size_t i;
	size_t o = 0;
	for (i = 0; i + 3 <= n; i += 3) {
		uint32_t v = ((uint32_t)in[i] << 16) |
		             ((uint32_t)in[i + 1] << 8) |
		             (uint32_t)in[i + 2];
		if (o + 4 > outcap) return 0;
		out[o++] = B64[(v >> 18) & 0x3f];
		out[o++] = B64[(v >> 12) & 0x3f];
		out[o++] = B64[(v >> 6) & 0x3f];
		out[o++] = B64[v & 0x3f];
	}
	if (i < n) {
		uint32_t v = (uint32_t)in[i] << 16;
		int rem = (int)(n - i);
		if (rem == 2) {
			v |= (uint32_t)in[i + 1] << 8;
		}
		if (o + 4 > outcap) return 0;
		out[o++] = B64[(v >> 18) & 0x3f];
		out[o++] = B64[(v >> 12) & 0x3f];
		out[o++] = (rem == 2) ? B64[(v >> 6) & 0x3f] : '=';
		out[o++] = '=';
	}
	if (o + 1 > outcap) return 0;
	out[o] = '\0';
	return o;
}

/* ---------------------------------------------------------------- *
 * main
 * ---------------------------------------------------------------- */

static int write_file(const char *path, const char *contents)
{
	FILE *fp = fopen(path, "wb");
	if (!fp) {
		fprintf(stderr, "gen-keypair: cannot write '%s': %s\n",
		        path, strerror(errno));
		return -1;
	}
	if (fputs(contents, fp) == EOF) {
		fclose(fp);
		return -1;
	}
	if (fclose(fp) != 0) {
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *seed_str;
	const char *pubkey_path;
	const char *seckey_path;
	const char *trusted_comment;
	uint8_t seed_hash[64];
	uint8_t pk[FTR_ED25519_PK_BYTES];
	uint8_t sk[64];
	uint8_t pub_blob[FTR_PUBKEY_RAW_BYTES];
	uint8_t sec_blob[2 /* sig alg */ + 2 /* kdf alg */ + 2 /* chk alg */
	                 + 32 /* kdf salt */ + 8 /* ops */ + 8 /* mem */
	                 + 8 /* keynum */ + 64 /* sk */ + 32 /* chk */];
	char b64_pub[128];
	char b64_sec[512];
	char pub_text[512];
	char sec_text[1024];
	uint8_t key_id[FTR_MINISIGN_KEYID_BYTES];
	char key_id_hex[17];
	size_t i;

	if (argc < 4 || argc > 5) {
		fprintf(stderr,
		        "Usage: gen-keypair <seed> <pubkey-out> "
		        "<seckey-out> [trusted-comment]\n");
		return 2;
	}
	seed_str = argv[1];
	pubkey_path = argv[2];
	seckey_path = argv[3];
	trusted_comment = (argc == 5) ? argv[4]
	                              : "feather phase 6 test keypair";

	/* Hash the seed string into 64 deterministic bytes; use the
	 * first 32 as the secret-key seed (matches the input
	 * crypto_sign_keypair pulls from randombytes). The remaining
	 * 32 bytes provide spare entropy in case TweetNaCl ever asks
	 * for more during keypair generation. */
	crypto_hash(seed_hash, (const unsigned char *)seed_str,
	            (unsigned long long)strlen(seed_str));
	g_seed_buf = seed_hash;
	g_seed_len = sizeof(seed_hash);
	g_seed_off = 0;

	if (crypto_sign_keypair(pk, sk) != 0) {
		fprintf(stderr, "gen-keypair: crypto_sign_keypair failed\n");
		return 1;
	}

	/* Derive a stable 8-byte key id from the public key. minisign
	 * itself draws this from randombytes, but a hash of the pubkey
	 * is equivalent for our deterministic-test purposes — two seeds
	 * that produce the same pk will produce the same key_id. */
	{
		uint8_t pk_hash[64];
		crypto_hash(pk_hash, pk, FTR_ED25519_PK_BYTES);
		memcpy(key_id, pk_hash, FTR_MINISIGN_KEYID_BYTES);
	}

	/* Compose the pubkey blob: alg(2) || key_id(8) || pk(32) */
	pub_blob[0] = (uint8_t)(FTR_MINISIGN_ALG_ED & 0xff);
	pub_blob[1] = (uint8_t)((FTR_MINISIGN_ALG_ED >> 8) & 0xff);
	memcpy(pub_blob + 2, key_id, FTR_MINISIGN_KEYID_BYTES);
	memcpy(pub_blob + 10, pk, FTR_ED25519_PK_BYTES);

	if (b64_encode(pub_blob, sizeof(pub_blob),
	               b64_pub, sizeof(b64_pub)) == 0) {
		fprintf(stderr, "gen-keypair: pubkey b64 buffer overflow\n");
		return 1;
	}

	/* Compute fingerprint for the comment line. */
	{
		static const char hex[] = "0123456789ABCDEF";
		for (i = 0; i < FTR_MINISIGN_KEYID_BYTES; i++) {
			uint8_t b = key_id[FTR_MINISIGN_KEYID_BYTES - 1 - i];
			key_id_hex[i * 2 + 0] = hex[(b >> 4) & 0x0f];
			key_id_hex[i * 2 + 1] = hex[b & 0x0f];
		}
		key_id_hex[16] = '\0';
	}

	(void)snprintf(pub_text, sizeof(pub_text),
	               "untrusted comment: minisign public key %s\n%s\n",
	               key_id_hex, b64_pub);

	/* Compose the seckey blob in minisign's unencrypted layout:
	 *
	 *   sig_alg(2)="Ed", kdf_alg(2)=0, chk_alg(2)="B2",
	 *   kdf_salt(32)=0, kdf_ops(8)=0, kdf_mem(8)=0,
	 *   keynum_sk[ key_id(8) || sk(64) || chk(32)=0 ]
	 *
	 * The checksum field is unused by our tools (we never feed our
	 * .sec file back into stock minisign at verify time), so we
	 * zero it.
	 */
	memset(sec_blob, 0, sizeof(sec_blob));
	sec_blob[0] = (uint8_t)(FTR_MINISIGN_ALG_ED & 0xff);
	sec_blob[1] = (uint8_t)((FTR_MINISIGN_ALG_ED >> 8) & 0xff);
	/* kdf_alg = 0x0000 (none) */
	sec_blob[4] = 'B';
	sec_blob[5] = '2';
	/* offset 6..38 kdf salt = 0 */
	/* offset 38..46 kdf ops = 0 */
	/* offset 46..54 kdf mem = 0 */
	memcpy(sec_blob + 54, key_id, FTR_MINISIGN_KEYID_BYTES);
	memcpy(sec_blob + 62, sk, 64);
	/* checksum bytes left zero */

	if (b64_encode(sec_blob, sizeof(sec_blob),
	               b64_sec, sizeof(b64_sec)) == 0) {
		fprintf(stderr, "gen-keypair: seckey b64 buffer overflow\n");
		return 1;
	}

	(void)snprintf(sec_text, sizeof(sec_text),
	               "untrusted comment: %s (seckey)\n%s\n",
	               trusted_comment, b64_sec);

	if (write_file(pubkey_path, pub_text) != 0) {
		return 1;
	}
	if (write_file(seckey_path, sec_text) != 0) {
		return 1;
	}
	/* Make seckey 0600 so tests don't trigger umask noise. */
	(void)chmod(seckey_path, 0600);

	printf("%s\n", key_id_hex);
	return 0;
}
