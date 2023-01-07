/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once

#include <getopt.h>
#include <stdbool.h>

struct OptionsExtra {
	const char* flags;
	struct option* options;
	bool (*parse)(void* userdata, int c);
	void (*free)(void* userdata);
	const char** usage;
	void* userdata;
};

struct Options {
	char* dev;
	char* name;
	bool usage;
	struct OptionsExtra* extra;
};

bool getopt_parse(int argc, char* argv[], struct Options*);
void getopt_free(struct Options*);
void usage(const char* arg0, bool help, struct OptionsExtra* extra);
