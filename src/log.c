/* SPDX-License-Identifier: BSD-3-Clause */
#include "log.h"

#include <stdarg.h>
#include <stdio.h>

static int _level = INFO;

__attribute__((format(printf, 2, 3)))
void log_fmt(enum LogLevel level, const char* fmt, ...) {
	if ((int) level > _level) {
		return;
	}
	va_list args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
}

void log_errno(enum LogLevel level, const char* msg) {
	if ((int) level > _level) {
		return;
	}
	perror(msg);
}

void set_log_level(enum LogLevel level) {
	_level = level;
}
