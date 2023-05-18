/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once

enum LogLevel {
	DEBUG = 3,
	INFO = 2,
	WARN = 1,
	ERROR = 0
};

__attribute__((format(printf, 2, 3)))
void log_fmt(enum LogLevel, const char* fmt, ...);
void log_errno(enum LogLevel, const char* msg);

void set_log_level(enum LogLevel);
