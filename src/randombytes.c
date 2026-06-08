/*
 * randombytes.c — implementation of the `randombytes()` symbol
 * declared by vendored TweetNaCl.
 *
 * The Ed25519 verify path (crypto_sign_open) does not invoke this
 * function — only keypair generation does. ftr itself never generates
 * keypairs at runtime; the symbol is here so static linking of
 * tweetnacl.o resolves cleanly when keypair functions are reachable
 * (e.g. from the gen-keypair test tool).
 *
 * Implementation reads from /dev/urandom and aborts on failure rather
 * than returning a zeroed buffer — cryptographic code that requests
 * randomness must never silently get zeros.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void randombytes(unsigned char *buf, unsigned long long n);

void randombytes(unsigned char *buf, unsigned long long n)
{
	int fd;
	unsigned long long off = 0;

	fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "ftr: error: cannot open /dev/urandom\n");
		exit(2);
	}
	while (off < n) {
		ssize_t r = read(fd, buf + off, (size_t)(n - off));
		if (r < 0) {
			if (errno == EINTR) {
				continue;
			}
			(void)close(fd);
			fprintf(stderr, "ftr: error: /dev/urandom read failed\n");
			exit(2);
		}
		if (r == 0) {
			(void)close(fd);
			fprintf(stderr, "ftr: error: /dev/urandom EOF\n");
			exit(2);
		}
		off += (unsigned long long)r;
	}
	(void)close(fd);
}
