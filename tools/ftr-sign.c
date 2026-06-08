/*
 * tools/ftr-sign.c — sign a file with a minisign-format secret key.
 *
 * Built outside the default `ftr` target; not installed. Used by
 * tests/phase6_signed_repo.sh to produce .feather.sig + index.toml.sig
 * sidecars from the keypair stamped out by tools/gen-keypair.
 *
 * Usage:
 *
 *   ftr-sign <seckey-path> <data-path> <sig-out-path> [trusted-comment]
 *
 * The seckey we accept is the unencrypted layout produced by
 * gen-keypair (kdf_alg = 0x0000). We don't support passphrase-encrypted
 * seckeys here — production signing happens on the build farm with
 * the real `minisign` binary.
 *
 * The resulting .sig file matches stock minisign's format so it can
 * also be verified with a system minisign install if anyone wants to
 * cross-check.
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
 * base64 (encode + decode)
 * ---------------------------------------------------------------- */

static const char B64[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_val(int c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

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
			if (pad) return -1;
			v = b64_val(c);
			if (v < 0) return -1;
			quad[qi++] = v;
		}
		if (qi == 4) {
			uint32_t accum = ((uint32_t)quad[0] << 18) |
			                 ((uint32_t)quad[1] << 12) |
			                 ((uint32_t)quad[2] << 6) |
			                 (uint32_t)quad[3];
			int produced = 3 - pad;
			if (n_out + (size_t)produced > outcap) return -1;
			out[n_out++] = (uint8_t)((accum >> 16) & 0xff);
			if (produced >= 2) {
				out[n_out++] = (uint8_t)((accum >> 8) & 0xff);
			}
			if (produced >= 3) {
				out[n_out++] = (uint8_t)(accum & 0xff);
			}
			qi = 0;
			if (pad) break;
		}
	}
	if (qi != 0) return -1;
	return (int)n_out;
}

/* ---------------------------------------------------------------- *
 * file IO
 * ---------------------------------------------------------------- */

static int slurp(const char *path, uint8_t **out_buf, size_t *out_len)
{
	FILE *fp;
	struct stat st;
	uint8_t *buf;
	size_t r;
	if (stat(path, &st) != 0) {
		fprintf(stderr, "ftr-sign: cannot stat '%s': %s\n",
		        path, strerror(errno));
		return -1;
	}
	fp = fopen(path, "rb");
	if (!fp) {
		fprintf(stderr, "ftr-sign: cannot open '%s'\n", path);
		return -1;
	}
	buf = malloc((size_t)st.st_size + 1);
	if (!buf) {
		fclose(fp);
		return -1;
	}
	r = fread(buf, 1, (size_t)st.st_size, fp);
	fclose(fp);
	if (r != (size_t)st.st_size) {
		free(buf);
		return -1;
	}
	buf[r] = '\0';
	*out_buf = buf;
	*out_len = r;
	return 0;
}

/* ---------------------------------------------------------------- *
 * seckey parser (matches the layout produced by tools/gen-keypair).
 *
 *   sig_alg(2)="Ed", kdf_alg(2)=0, chk_alg(2)="B2",
 *   kdf_salt(32), kdf_ops(8), kdf_mem(8),
 *   key_id(8), sk(64), chk(32)
 *
 * Output: key_id (8 bytes), sk (64 bytes).
 * ---------------------------------------------------------------- */

static int parse_seckey(const char *path, uint8_t key_id[8], uint8_t sk[64])
{
	uint8_t *buf = NULL;
	size_t len = 0;
	const char *p;
	const char *eol;
	uint8_t blob[256];
	int n;
	if (slurp(path, &buf, &len) != 0) {
		return -1;
	}
	/* line 1: untrusted comment */
	p = (const char *)buf;
	eol = strchr(p, '\n');
	if (!eol) {
		fprintf(stderr, "ftr-sign: seckey missing first line\n");
		free(buf);
		return -1;
	}
	p = eol + 1;
	/* line 2: base64 of seckey blob */
	eol = strchr(p, '\n');
	if (!eol) {
		fprintf(stderr, "ftr-sign: seckey missing data line\n");
		free(buf);
		return -1;
	}
	n = b64_decode(p, (size_t)(eol - p), blob, sizeof(blob));
	free(buf);
	if (n < 62 + 64) {
		fprintf(stderr,
		        "ftr-sign: seckey too short (decoded %d bytes)\n", n);
		return -1;
	}
	if (blob[0] != (uint8_t)(FTR_MINISIGN_ALG_ED & 0xff) ||
	    blob[1] != (uint8_t)((FTR_MINISIGN_ALG_ED >> 8) & 0xff)) {
		fprintf(stderr,
		        "ftr-sign: seckey has unexpected sig_alg tag\n");
		return -1;
	}
	if (blob[2] != 0 || blob[3] != 0) {
		fprintf(stderr,
		        "ftr-sign: seckey is passphrase-encrypted; "
		        "this tool only handles unencrypted keys\n");
		return -1;
	}
	memcpy(key_id, blob + 54, 8);
	memcpy(sk, blob + 62, 64);
	return 0;
}

/* ---------------------------------------------------------------- *
 * sign
 * ---------------------------------------------------------------- */

static int write_file(const char *path, const char *text)
{
	FILE *fp = fopen(path, "wb");
	if (!fp) {
		fprintf(stderr, "ftr-sign: cannot write '%s': %s\n",
		        path, strerror(errno));
		return -1;
	}
	if (fputs(text, fp) == EOF) {
		fclose(fp);
		return -1;
	}
	if (fclose(fp) != 0) return -1;
	return 0;
}

int main(int argc, char **argv)
{
	const char *seckey_path;
	const char *data_path;
	const char *sig_out_path;
	const char *trusted_comment;
	uint8_t key_id[8];
	uint8_t sk[64];
	uint8_t *data = NULL;
	size_t data_len = 0;
	uint8_t *signed_msg = NULL;
	uint8_t *signed_global = NULL;
	unsigned long long sm_len = 0;
	unsigned long long sg_len = 0;
	uint8_t sig_blob[FTR_SIG_RAW_BYTES];
	uint8_t gsig[FTR_GLOBAL_SIG_BYTES];
	char b64_sig[256];
	char b64_gsig[256];
	char out[4096];
	uint8_t *gmsg_buf = NULL;
	uint8_t *gout_buf = NULL;
	size_t gmsg_len;
	int rc = 1;

	if (argc < 4 || argc > 5) {
		fprintf(stderr,
		        "Usage: ftr-sign <seckey-path> <data-path> "
		        "<sig-out-path> [trusted-comment]\n");
		return 2;
	}
	seckey_path = argv[1];
	data_path = argv[2];
	sig_out_path = argv[3];
	trusted_comment = (argc == 5)
	                  ? argv[4]
	                  : "timestamp:0\tfile:placeholder";

	if (parse_seckey(seckey_path, key_id, sk) != 0) {
		return 1;
	}
	if (slurp(data_path, &data, &data_len) != 0) {
		return 1;
	}

	/* Primary signature over the data bytes. crypto_sign emits
	 * sig(64) || msg, which we split: keep the 64-byte sig only. */
	sm_len = data_len + 64;
	signed_msg = malloc((size_t)sm_len);
	if (!signed_msg) {
		fprintf(stderr, "ftr-sign: oom\n");
		goto out;
	}
	if (crypto_sign(signed_msg, &sm_len, data, (unsigned long long)data_len,
	                sk) != 0) {
		fprintf(stderr, "ftr-sign: crypto_sign (primary) failed\n");
		goto out;
	}

	/* Compose the 74-byte sig blob: alg || key_id || sig */
	sig_blob[0] = (uint8_t)(FTR_MINISIGN_ALG_ED & 0xff);
	sig_blob[1] = (uint8_t)((FTR_MINISIGN_ALG_ED >> 8) & 0xff);
	memcpy(sig_blob + 2, key_id, 8);
	memcpy(sig_blob + 10, signed_msg, 64);

	/* Global signature over sig(64) || trusted_comment. */
	{
		size_t tc_len = strlen(trusted_comment);
		gmsg_len = 64 + tc_len;
		gmsg_buf = malloc(gmsg_len);
		gout_buf = malloc(gmsg_len + 64);
		if (!gmsg_buf || !gout_buf) {
			fprintf(stderr, "ftr-sign: oom\n");
			goto out;
		}
		memcpy(gmsg_buf, signed_msg, 64);
		memcpy(gmsg_buf + 64, trusted_comment, tc_len);
		sg_len = gmsg_len + 64;
		if (crypto_sign(gout_buf, &sg_len, gmsg_buf,
		                (unsigned long long)gmsg_len, sk) != 0) {
			fprintf(stderr,
			        "ftr-sign: crypto_sign (global) failed\n");
			goto out;
		}
		memcpy(gsig, gout_buf, 64);
	}

	if (b64_encode(sig_blob, sizeof(sig_blob),
	               b64_sig, sizeof(b64_sig)) == 0) {
		fprintf(stderr, "ftr-sign: b64 overflow on sig\n");
		goto out;
	}
	if (b64_encode(gsig, sizeof(gsig),
	               b64_gsig, sizeof(b64_gsig)) == 0) {
		fprintf(stderr, "ftr-sign: b64 overflow on global sig\n");
		goto out;
	}

	(void)snprintf(out, sizeof(out),
	               "untrusted comment: signature from feather test key\n"
	               "%s\n"
	               "trusted comment: %s\n"
	               "%s\n",
	               b64_sig, trusted_comment, b64_gsig);

	if (write_file(sig_out_path, out) != 0) {
		goto out;
	}
	rc = 0;

out:
	free(data);
	free(signed_msg);
	free(signed_global);
	free(gmsg_buf);
	free(gout_buf);
	return rc;
}
