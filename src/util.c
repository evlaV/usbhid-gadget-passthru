/* SPDX-License-Identifier: BSD-3-Clause */
#include <dirent.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util.h"

__attribute__((format(printf, 1, 3)))
int vmkdir(const char* pattern, int mode, ...) {
	char path[PATH_MAX];
	va_list args;
	va_start(args, mode);
	vsnprintf(path, sizeof(path), pattern, args);
	va_end(args);
	return mkdir(path, mode);
}

__attribute__((format(printf, 1, 4)))
int vopen(const char* pattern, int flags, int mode, ...) {
	char path[PATH_MAX];
	va_list args;
	va_start(args, mode);
	vsnprintf(path, sizeof(path), pattern, args);
	va_end(args);
	return open(path, flags, mode);
}

bool cp_prop(const char* restrict indir, const char* inpath, const char* restrict outdir, const char* outpath) {
	char in[PATH_MAX];
	char out[PATH_MAX];
	char buf[2048];
	ssize_t size;
	int infd = -1;
	int outfd = -1;

	snprintf(in, sizeof(in), "%s/%s", indir, inpath);
	snprintf(out, sizeof(out), "%s/%s", outdir, outpath);
	infd = open(in, O_RDONLY);
	outfd = open(out, O_WRONLY | O_TRUNC, 0644);

	if (infd < 0) {
		perror("Failed to open property input");
		close(outfd);
		return false;
	}

	if (outfd < 0) {
		perror("Failed to open property output");
		close(infd);
		return false;
	}

	while ((size = read(infd, buf, sizeof(buf))) > 0) {
		if (write(outfd, buf, size) != size) {
			perror("Failed to copy property");
			close(infd);
			close(outfd);
			return false;
		}
	}
	close(infd);
	close(outfd);
	return true;
}

bool cp_prop_hex(const char* restrict indir, const char* inpath, const char* restrict outdir, const char* outpath) {
	char in[PATH_MAX];
	char out[PATH_MAX];
	char buf[2048];
	ssize_t size;
	int infd = -1;
	int outfd = -1;

	snprintf(in, sizeof(in), "%s/%s", indir, inpath);
	snprintf(out, sizeof(out), "%s/%s", outdir, outpath);
	infd = open(in, O_RDONLY);
	outfd = open(out, O_WRONLY | O_TRUNC, 0644);

	if (infd < 0) {
		perror("Failed to open property input");
		close(outfd);
		return false;
	}

	if (outfd < 0) {
		perror("Failed to open property output");
		close(infd);
		return false;
	}

	buf[0] = '0';
	buf[1] = 'x';
	size = read(infd, &buf[2], sizeof(buf) - 2);
	if (size < 1) {
		perror("Failed to read property");
		close(infd);
		close(outfd);
		return false;
	}
	size += 2;

	if (write(outfd, buf, size) != size) {
		perror("Failed to write property");
		close(infd);
		close(outfd);
		return false;
	}

	close(infd);
	close(outfd);
	return true;
}

