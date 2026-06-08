/*
 * verify.c — minisign-format Ed25519 signature verification.
 *
 * Surface contract is in verify.h. Internally this file owns:
 *
 *   - base64 decode (RFC 4648, no '\n' tolerance, padding required)
 *   - minisign file parsers (pubkey + .sig)
 *   - archive-streaming Ed25519 verify (via TweetNaCl crypto_sign_open)
 *   - the FTR_DEFAULT_PUBKEY placeholder constant
 *   - the pubkey-resolution chain (per-repo file → $FTR_PUBKEY → default)
 *
 * Verification path is fail-closed: anything we don't recognise (alg
 * tag, base64 length, signature mismatch, hash mismatch) returns -1
 * with errbuf populated. Repo install bails on any verify failure;
 * local install allows opt-in unsigned via --allow-unsigned (handled
 * in cmd_install.c).
 */

#define _POSIX_C_SOURCE 200809L

#include "verify.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tweetnacl.h"

/* ---------------------------------------------------------------- *
 * FTR_DEFAULT_PUBKEY — PHASE 6 PLACEHOLDER KEY
 *
 * Generated deterministically by tools/gen-keypair from the seed
 * "feather-phase6-placeholder-seed-do-not-ship" (see
 * tests/data/test_seckey.txt for the matching secret half). The
 * production build farm key will replace this constant before any
 * tagged feather release ships to users.
 * ---------------------------------------------------------------- */

const char FTR_DEFAULT_PUBKEY[] =
	"untrusted comment: minisign public key 235EEAE7A4900A83\n"
	"ZEWDCpCk5+peI+3cWcBs5EA7COaOjZz3tLJZ8DYK8bFokcVUS6B6HUpj\n";

/* ---------------------------------------------------------------- *
 * base64 decode (RFC 4648, padded)
 * ---------------------------------------------------------------- */

static int b64_val(int c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

/* Decode base64 string `in` (length `inlen`) into `out` (capacity
 * `outcap`). Returns the number of bytes written, or -1 on malformed
 * input or insufficient output buffer. Whitespace inside the input is
 * tolerated. */
static int b64_decode(const char *in, size_t inlen,
                      uint8_t *out, size_t outcap)
{
	size_t i;
	size_t n_out = 0;
	int quad[4];
	int qi = 0;
	int pad = 0;
	for (i = 0; i < inlen; i++) {
		int c = (unsigned char)in[i];
		if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
			continue;
		}
		if (c == '=') {
			pad++;
			quad[qi++] = 0;
		} else {
			int v;
			if (pad) {
				return -1; /* non-pad after pad */
			}
			v = b64_val(c);
			if (v < 0) {
				return -1;
			}
			quad[qi++] = v;
		}
		if (qi == 4) {
			uint32_t accum = ((uint32_t)quad[0] << 18) |
			                 ((uint32_t)quad[1] << 12) |
			                 ((uint32_t)quad[2] << 6) |
			                 (uint32_t)quad[3];
			int produced = 3 - pad;
			if (produced < 1) {
				return -1;
			}
			if (n_out + (size_t)produced > outcap) {
				return -1;
			}
			out[n_out++] = (uint8_t)((accum >> 16) & 0xff);
			if (produced >= 2) {
				out[n_out++] = (uint8_t)((accum >> 8) & 0xff);
			}
			if (produced >= 3) {
				out[n_out++] = (uint8_t)(accum & 0xff);
			}
			qi = 0;
			if (pad) {
				break;
			}
		}
	}
	if (qi != 0) {
		return -1;
	}
	return (int)n_out;
}

/* ---------------------------------------------------------------- *
 * tiny errbuf helper
 * ---------------------------------------------------------------- */

static void verr(char *errbuf, size_t errbufsz, const char *fmt, ...)
{
	va_list ap;
	if (!errbuf || errbufsz == 0) {
		return;
	}
	va_start(ap, fmt);
	(void)vsnprintf(errbuf, errbufsz, fmt, ap);
	va_end(ap);
	errbuf[errbufsz - 1] = '\0';
}

/* ---------------------------------------------------------------- *
 * file slurp
 * ---------------------------------------------------------------- */

static int read_text_file(const char *path, char **out_buf, size_t *out_len,
                          char *errbuf, size_t errbufsz)
{
	FILE *fp;
	long sz;
	char *buf;
	size_t r;

	fp = fopen(path, "rb");
	if (!fp) {
		verr(errbuf, errbufsz, "cannot open '%s': %s", path,
		     strerror(errno));
		return -1;
	}
	if (fseek(fp, 0, SEEK_END) != 0) {
		verr(errbuf, errbufsz, "fseek '%s' failed", path);
		fclose(fp);
		return -1;
	}
	sz = ftell(fp);
	if (sz < 0 || sz > (long)(64 * 1024)) {
		verr(errbuf, errbufsz, "'%s' too large to be a minisign file",
		     path);
		fclose(fp);
		return -1;
	}
	rewind(fp);
	buf = malloc((size_t)sz + 1);
	if (!buf) {
		verr(errbuf, errbufsz, "out of memory");
		fclose(fp);
		return -1;
	}
	r = fread(buf, 1, (size_t)sz, fp);
	fclose(fp);
	if (r != (size_t)sz) {
		verr(errbuf, errbufsz, "short read on '%s'", path);
		free(buf);
		return -1;
	}
	buf[sz] = '\0';
	*out_buf = buf;
	*out_len = (size_t)sz;
	return 0;
}

/* ---------------------------------------------------------------- *
 * line walker
 *
 * Returns a pointer to the next '\n' (or end of buffer) and writes
 * the start of the line into *out_start, its length (without the
 * '\n') into *out_len. Returns NULL when *p is at EOF.
 * ---------------------------------------------------------------- */

static const char *next_line(const char *p, const char *end,
                             const char **out_start, size_t *out_len)
{
	const char *eol;
	if (p >= end) {
		return NULL;
	}
	*out_start = p;
	eol = p;
	while (eol < end && *eol != '\n') {
		eol++;
	}
	*out_len = (size_t)(eol - p);
	/* trim trailing '\r' */
	if (*out_len > 0 && p[*out_len - 1] == '\r') {
		(*out_len)--;
	}
	if (eol < end) {
		eol++; /* skip '\n' */
	}
	return eol;
}

/* ---------------------------------------------------------------- *
 * pubkey parser
 * ---------------------------------------------------------------- */

int ftr_verify_parse_pubkey(const char *text, ftr_pubkey *out,
                            char *errbuf, size_t errbufsz)
{
	const char *p = text;
	const char *end;
	const char *line;
	size_t line_len;
	uint8_t raw[FTR_PUBKEY_RAW_BYTES];
	int n;

	if (!text || !out) {
		verr(errbuf, errbufsz, "null input");
		return -1;
	}
	end = text + strlen(text);
	memset(out, 0, sizeof(*out));

	/* line 1: "untrusted comment: ..." */
	p = next_line(p, end, &line, &line_len);
	if (!p) {
		verr(errbuf, errbufsz,
		     "minisign pubkey: missing untrusted comment");
		return -1;
	}
	if (line_len < 18 ||
	    strncmp(line, "untrusted comment:", 18) != 0) {
		verr(errbuf, errbufsz,
		     "minisign pubkey: first line must start with "
		     "'untrusted comment:'");
		return -1;
	}

	/* line 2: base64 of 42-byte blob */
	p = next_line(p, end, &line, &line_len);
	if (!p) {
		verr(errbuf, errbufsz,
		     "minisign pubkey: missing key data line");
		return -1;
	}
	n = b64_decode(line, line_len, raw, sizeof(raw));
	if (n != FTR_PUBKEY_RAW_BYTES) {
		verr(errbuf, errbufsz,
		     "minisign pubkey: expected %d decoded bytes, got %d",
		     FTR_PUBKEY_RAW_BYTES, n);
		return -1;
	}

	out->alg = (uint16_t)raw[0] | ((uint16_t)raw[1] << 8);
	if (out->alg == FTR_MINISIGN_ALG_HASHED) {
		verr(errbuf, errbufsz,
		     "unsupported signature algorithm (legacy hashed 'ED')");
		return -1;
	}
	if (out->alg != FTR_MINISIGN_ALG_ED) {
		verr(errbuf, errbufsz,
		     "unsupported signature algorithm (tag=0x%04x)",
		     (unsigned)out->alg);
		return -1;
	}
	memcpy(out->key_id, raw + 2, FTR_MINISIGN_KEYID_BYTES);
	memcpy(out->pk, raw + 10, FTR_ED25519_PK_BYTES);
	return 0;
}

int ftr_verify_load_pubkey(const char *path, ftr_pubkey *out,
                           char *errbuf, size_t errbufsz)
{
	char *buf = NULL;
	size_t len = 0;
	int rc;
	if (read_text_file(path, &buf, &len, errbuf, errbufsz) != 0) {
		return -1;
	}
	rc = ftr_verify_parse_pubkey(buf, out, errbuf, errbufsz);
	free(buf);
	return rc;
}

/* ---------------------------------------------------------------- *
 * signature parser
 * ---------------------------------------------------------------- */

int ftr_verify_load_signature(const char *path, ftr_signature *out,
                              char *errbuf, size_t errbufsz)
{
	char *buf = NULL;
	size_t len = 0;
	const char *p;
	const char *end;
	const char *line;
	size_t line_len;
	uint8_t raw[FTR_SIG_RAW_BYTES];
	uint8_t gsig[FTR_GLOBAL_SIG_BYTES];
	int n;

	if (read_text_file(path, &buf, &len, errbuf, errbufsz) != 0) {
		return -1;
	}
	memset(out, 0, sizeof(*out));
	p = buf;
	end = buf + len;

	/* line 1: "untrusted comment: ..." */
	p = next_line(p, end, &line, &line_len);
	if (!p || line_len < 18 ||
	    strncmp(line, "untrusted comment:", 18) != 0) {
		verr(errbuf, errbufsz,
		     "minisign sig: missing 'untrusted comment:' header");
		goto fail;
	}

	/* line 2: 74-byte signature blob, base64 */
	p = next_line(p, end, &line, &line_len);
	if (!p) {
		verr(errbuf, errbufsz, "minisign sig: missing signature line");
		goto fail;
	}
	n = b64_decode(line, line_len, raw, sizeof(raw));
	if (n != FTR_SIG_RAW_BYTES) {
		verr(errbuf, errbufsz,
		     "minisign sig: expected %d decoded bytes, got %d",
		     FTR_SIG_RAW_BYTES, n);
		goto fail;
	}
	out->alg = (uint16_t)raw[0] | ((uint16_t)raw[1] << 8);
	if (out->alg == FTR_MINISIGN_ALG_HASHED) {
		verr(errbuf, errbufsz,
		     "unsupported signature algorithm (legacy hashed 'ED')");
		goto fail;
	}
	if (out->alg != FTR_MINISIGN_ALG_ED) {
		verr(errbuf, errbufsz,
		     "unsupported signature algorithm (tag=0x%04x)",
		     (unsigned)out->alg);
		goto fail;
	}
	memcpy(out->key_id, raw + 2, FTR_MINISIGN_KEYID_BYTES);
	memcpy(out->sig, raw + 10, FTR_ED25519_SIG_BYTES);

	/* line 3: "trusted comment: ..." */
	p = next_line(p, end, &line, &line_len);
	if (!p || line_len < 16 ||
	    strncmp(line, "trusted comment:", 16) != 0) {
		verr(errbuf, errbufsz,
		     "minisign sig: missing 'trusted comment:' line");
		goto fail;
	}
	{
		const char *tc = line + 16;
		size_t tc_len = line_len - 16;
		if (tc_len > 0 && *tc == ' ') {
			tc++;
			tc_len--;
		}
		if (tc_len >= sizeof(out->trusted_comment)) {
			verr(errbuf, errbufsz,
			     "minisign sig: trusted comment too long (%zu)",
			     tc_len);
			goto fail;
		}
		memcpy(out->trusted_comment, tc, tc_len);
		out->trusted_comment[tc_len] = '\0';
		out->trusted_comment_len = tc_len;
	}

	/* line 4: 64-byte global signature, base64 */
	p = next_line(p, end, &line, &line_len);
	if (!p) {
		verr(errbuf, errbufsz,
		     "minisign sig: missing global signature line");
		goto fail;
	}
	n = b64_decode(line, line_len, gsig, sizeof(gsig));
	if (n != FTR_GLOBAL_SIG_BYTES) {
		verr(errbuf, errbufsz,
		     "minisign sig: global sig wrong size (%d)", n);
		goto fail;
	}
	memcpy(out->global_sig, gsig, FTR_GLOBAL_SIG_BYTES);

	free(buf);
	return 0;

fail:
	free(buf);
	memset(out, 0, sizeof(*out));
	return -1;
}

/* ---------------------------------------------------------------- *
 * fingerprint helper
 * ---------------------------------------------------------------- */

void ftr_pubkey_fingerprint(const ftr_pubkey *pk, char out[17])
{
	int i;
	static const char hex[] = "0123456789ABCDEF";
	/* minisign convention: little-endian key_id, hex-encoded with
	 * the high byte first. We display bytes left-to-right which
	 * lines up with `minisign -G` output. */
	for (i = 0; i < FTR_MINISIGN_KEYID_BYTES; i++) {
		uint8_t b = pk->key_id[FTR_MINISIGN_KEYID_BYTES - 1 - i];
		out[i * 2 + 0] = hex[(b >> 4) & 0x0f];
		out[i * 2 + 1] = hex[b & 0x0f];
	}
	out[16] = '\0';
}

/* ---------------------------------------------------------------- *
 * archive verify
 *
 * Strategy: build the "signed message" expected by TweetNaCl
 * (sig || data), call crypto_sign_open. Then build the global signed
 * message (gsig || sig || trusted_comment) and verify that too.
 * ---------------------------------------------------------------- */

/* Slurp an archive file into a malloc'd buffer. Archives are small
 * (kilobytes in tests, megabytes in prod) — fine to read into RAM
 * for verify. */
static int slurp_binary(const char *path, uint8_t **out_buf,
                        size_t *out_len,
                        char *errbuf, size_t errbufsz)
{
	FILE *fp;
	struct stat st;
	uint8_t *buf;
	size_t r;

	if (stat(path, &st) != 0) {
		verr(errbuf, errbufsz, "cannot stat '%s': %s", path,
		     strerror(errno));
		return -1;
	}
	if (st.st_size < 0 || (uint64_t)st.st_size > (uint64_t)(512 * 1024 * 1024)) {
		verr(errbuf, errbufsz,
		     "archive '%s' too large to verify (limit 512 MiB)", path);
		return -1;
	}
	fp = fopen(path, "rb");
	if (!fp) {
		verr(errbuf, errbufsz, "cannot open '%s': %s", path,
		     strerror(errno));
		return -1;
	}
	buf = malloc((size_t)st.st_size + 1);
	if (!buf) {
		fclose(fp);
		verr(errbuf, errbufsz, "out of memory");
		return -1;
	}
	r = fread(buf, 1, (size_t)st.st_size, fp);
	fclose(fp);
	if (r != (size_t)st.st_size) {
		free(buf);
		verr(errbuf, errbufsz, "short read on '%s'", path);
		return -1;
	}
	*out_buf = buf;
	*out_len = r;
	return 0;
}

int ftr_verify_archive(const char *archive_path,
                       const ftr_signature *sig,
                       const ftr_pubkey *pk,
                       char *errbuf, size_t errbufsz)
{
	uint8_t *arch_buf = NULL;
	size_t arch_len = 0;
	uint8_t *sm = NULL;       /* signed message [sig || data] */
	uint8_t *m = NULL;        /* recovered message */
	unsigned long long smlen;
	unsigned long long mlen;
	uint8_t *gsm = NULL;      /* global signed message */
	uint8_t *gm = NULL;
	unsigned long long gsmlen;
	unsigned long long gmlen;
	int rc = -1;

	if (!archive_path || !sig || !pk) {
		verr(errbuf, errbufsz, "null argument");
		return -1;
	}

	/* Key-id sanity: if sig was signed by a different key, fail
	 * with a clear diagnostic so the user knows which key to
	 * trust. */
	if (memcmp(sig->key_id, pk->key_id, FTR_MINISIGN_KEYID_BYTES) != 0) {
		char want[17];
		char got[17];
		ftr_pubkey ghost;
		memset(&ghost, 0, sizeof(ghost));
		memcpy(ghost.key_id, sig->key_id, FTR_MINISIGN_KEYID_BYTES);
		ftr_pubkey_fingerprint(pk, want);
		ftr_pubkey_fingerprint(&ghost, got);
		verr(errbuf, errbufsz,
		     "signature key id mismatch: expected %s, sig is %s",
		     want, got);
		return -1;
	}

	if (slurp_binary(archive_path, &arch_buf, &arch_len,
	                 errbuf, errbufsz) != 0) {
		return -1;
	}

	/* Build signed message: 64-byte sig || archive bytes. */
	smlen = (unsigned long long)FTR_ED25519_SIG_BYTES + (unsigned long long)arch_len;
	sm = malloc((size_t)smlen);
	m  = malloc((size_t)smlen);
	if (!sm || !m) {
		verr(errbuf, errbufsz, "out of memory");
		goto out;
	}
	memcpy(sm, sig->sig, FTR_ED25519_SIG_BYTES);
	memcpy(sm + FTR_ED25519_SIG_BYTES, arch_buf, arch_len);

	if (crypto_sign_open(m, &mlen, sm, smlen, pk->pk) != 0) {
		verr(errbuf, errbufsz,
		     "archive signature verification failed (Ed25519)");
		goto out;
	}

	/* Global signature: covers sig||trusted_comment so a tampered
	 * trusted_comment is detected. */
	gsmlen = (unsigned long long)FTR_ED25519_SIG_BYTES
	         + (unsigned long long)FTR_ED25519_SIG_BYTES
	         + (unsigned long long)sig->trusted_comment_len;
	gsm = malloc((size_t)gsmlen);
	gm  = malloc((size_t)gsmlen);
	if (!gsm || !gm) {
		verr(errbuf, errbufsz, "out of memory");
		goto out;
	}
	memcpy(gsm, sig->global_sig, FTR_ED25519_SIG_BYTES);
	memcpy(gsm + FTR_ED25519_SIG_BYTES, sig->sig, FTR_ED25519_SIG_BYTES);
	memcpy(gsm + 2 * FTR_ED25519_SIG_BYTES, sig->trusted_comment,
	       sig->trusted_comment_len);

	if (crypto_sign_open(gm, &gmlen, gsm, gsmlen, pk->pk) != 0) {
		verr(errbuf, errbufsz,
		     "trusted-comment signature verification failed");
		goto out;
	}

	rc = 0;

out:
	free(arch_buf);
	free(sm);
	free(m);
	free(gsm);
	free(gm);
	return rc;
}

/* ---------------------------------------------------------------- *
 * pubkey resolution
 *
 * Precedence:
 *   1. caller-supplied per-repo pubkey file (from feather.conf)
 *   2. $FTR_PUBKEY env var path
 *   3. compile-time FTR_DEFAULT_PUBKEY
 * ---------------------------------------------------------------- */

int ftr_verify_resolve_pubkey(const char *pubkey_path,
                              ftr_pubkey *out,
                              char *errbuf, size_t errbufsz)
{
	const char *env_path;
	if (pubkey_path && *pubkey_path) {
		return ftr_verify_load_pubkey(pubkey_path, out,
		                              errbuf, errbufsz);
	}
	env_path = getenv("FTR_PUBKEY");
	if (env_path && *env_path) {
		return ftr_verify_load_pubkey(env_path, out,
		                              errbuf, errbufsz);
	}
	return ftr_verify_parse_pubkey(FTR_DEFAULT_PUBKEY, out,
	                               errbuf, errbufsz);
}
