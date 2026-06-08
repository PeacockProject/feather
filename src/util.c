/*
 * util.c — small filesystem / string helpers shared by db.c +
 * install.c. POSIX.1-2008.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE /* DT_DIR / DT_LNK from <dirent.h> on glibc */

#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ---------------------------------------------------------------- *
 * path_join
 * ---------------------------------------------------------------- */

char *path_join(int n, ...)
{
	va_list ap;
	int i;
	size_t total = 1; /* trailing NUL */
	char *out;
	char *p;
	const char **parts;

	if (n <= 0) {
		return NULL;
	}
	parts = calloc((size_t)n, sizeof(*parts));
	if (!parts) {
		return NULL;
	}
	va_start(ap, n);
	for (i = 0; i < n; i++) {
		parts[i] = va_arg(ap, const char *);
		if (parts[i] && parts[i][0] != '\0') {
			total += strlen(parts[i]) + 1; /* +1 for separator */
		}
	}
	va_end(ap);

	out = malloc(total);
	if (!out) {
		free(parts);
		return NULL;
	}
	p = out;
	*p = '\0';
	for (i = 0; i < n; i++) {
		const char *s = parts[i];
		size_t slen;
		if (!s || !*s) {
			continue;
		}
		slen = strlen(s);
		if (p == out) {
			/* first non-empty component: preserve a leading '/'
			 * if present. */
			memcpy(p, s, slen);
			p += slen;
		} else {
			/* skip leading '/' on subsequent components, and
			 * trim a trailing '/' from the existing tail. */
			while (p > out && *(p - 1) == '/') {
				p--;
			}
			while (*s == '/') {
				s++;
				slen--;
			}
			*p++ = '/';
			memcpy(p, s, slen);
			p += slen;
		}
	}
	*p = '\0';
	free(parts);
	return out;
}

/* ---------------------------------------------------------------- *
 * mkdir_p
 * ---------------------------------------------------------------- */

int mkdir_p(const char *path, mode_t mode)
{
	char *tmp;
	char *p;
	size_t len;

	if (!path || !*path) {
		errno = EINVAL;
		return -1;
	}

	len = strlen(path);
	tmp = malloc(len + 1);
	if (!tmp) {
		return -1;
	}
	memcpy(tmp, path, len + 1);

	/* Iterate over each '/' boundary and mkdir the prefix. */
	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
				free(tmp);
				return -1;
			}
			*p = '/';
		}
	}
	if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
		free(tmp);
		return -1;
	}
	free(tmp);
	return 0;
}

/* ---------------------------------------------------------------- *
 * rm_rf
 * ---------------------------------------------------------------- */

int rm_rf(const char *path)
{
	DIR *d;
	struct dirent *de;
	struct stat st;

	if (lstat(path, &st) != 0) {
		if (errno == ENOENT) {
			return 0;
		}
		return -1;
	}
	if (!S_ISDIR(st.st_mode)) {
		return unlink(path) == 0 ? 0 : -1;
	}

	d = opendir(path);
	if (!d) {
		return -1;
	}
	while ((de = readdir(d)) != NULL) {
		char *child;
		if (strcmp(de->d_name, ".") == 0 ||
		    strcmp(de->d_name, "..") == 0) {
			continue;
		}
		child = path_join(2, path, de->d_name);
		if (!child) {
			closedir(d);
			return -1;
		}
		if (rm_rf(child) != 0) {
			free(child);
			closedir(d);
			return -1;
		}
		free(child);
	}
	closedir(d);
	return rmdir(path) == 0 ? 0 : -1;
}

/* ---------------------------------------------------------------- *
 * copy_file
 * ---------------------------------------------------------------- */

int copy_file(const char *src, const char *dst)
{
	int in_fd = -1;
	int out_fd = -1;
	struct stat st;
	char buf[8192];
	ssize_t n;

	in_fd = open(src, O_RDONLY);
	if (in_fd < 0) {
		return -1;
	}
	if (fstat(in_fd, &st) != 0) {
		close(in_fd);
		return -1;
	}
	out_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
	if (out_fd < 0) {
		close(in_fd);
		return -1;
	}
	while ((n = read(in_fd, buf, sizeof(buf))) > 0) {
		ssize_t off = 0;
		while (off < n) {
			ssize_t w = write(out_fd, buf + off, (size_t)(n - off));
			if (w < 0) {
				if (errno == EINTR) {
					continue;
				}
				close(in_fd);
				close(out_fd);
				return -1;
			}
			off += w;
		}
	}
	close(in_fd);
	if (close(out_fd) != 0) {
		return -1;
	}
	if (n < 0) {
		return -1;
	}
	return 0;
}

/* ---------------------------------------------------------------- *
 * copy_tree
 * ---------------------------------------------------------------- */

int copy_tree(const char *src, const char *dst)
{
	DIR *d;
	struct dirent *de;
	struct stat src_st;

	if (lstat(src, &src_st) != 0) {
		return -1;
	}
	if (!S_ISDIR(src_st.st_mode)) {
		errno = ENOTDIR;
		return -1;
	}

	if (mkdir_p(dst, 0755) != 0) {
		return -1;
	}

	d = opendir(src);
	if (!d) {
		return -1;
	}
	while ((de = readdir(d)) != NULL) {
		char *src_child = NULL;
		char *dst_child = NULL;
		struct stat st;

		if (strcmp(de->d_name, ".") == 0 ||
		    strcmp(de->d_name, "..") == 0) {
			continue;
		}
		src_child = path_join(2, src, de->d_name);
		dst_child = path_join(2, dst, de->d_name);
		if (!src_child || !dst_child) {
			free(src_child);
			free(dst_child);
			closedir(d);
			return -1;
		}
		if (lstat(src_child, &st) != 0) {
			free(src_child);
			free(dst_child);
			closedir(d);
			return -1;
		}
		if (S_ISDIR(st.st_mode)) {
			if (copy_tree(src_child, dst_child) != 0) {
				free(src_child);
				free(dst_child);
				closedir(d);
				return -1;
			}
		} else if (S_ISLNK(st.st_mode)) {
			char target[4096];
			ssize_t len = readlink(src_child, target,
			                       sizeof(target) - 1);
			if (len < 0) {
				free(src_child);
				free(dst_child);
				closedir(d);
				return -1;
			}
			target[len] = '\0';
			(void)unlink(dst_child); /* tolerate pre-existing */
			if (symlink(target, dst_child) != 0) {
				free(src_child);
				free(dst_child);
				closedir(d);
				return -1;
			}
		} else if (S_ISREG(st.st_mode)) {
			if (copy_file(src_child, dst_child) != 0) {
				free(src_child);
				free(dst_child);
				closedir(d);
				return -1;
			}
		}
		/* sockets / fifos / devs: silently skip — not expected
		 * in a phase-4 .feather archive. */
		free(src_child);
		free(dst_child);
	}
	closedir(d);
	return 0;
}

/* ---------------------------------------------------------------- *
 * tiny helpers
 * ---------------------------------------------------------------- */

int strcmp_qsort(const void *a, const void *b)
{
	const char *sa = *(const char *const *)a;
	const char *sb = *(const char *const *)b;
	return strcmp(sa, sb);
}

int path_exists(const char *path)
{
	struct stat st;
	return lstat(path, &st) == 0 ? 1 : 0;
}

int is_dir(const char *path)
{
	struct stat st;
	if (stat(path, &st) != 0) {
		return 0;
	}
	return S_ISDIR(st.st_mode) ? 1 : 0;
}
