/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once

#include <getopt.h>
#include <stdbool.h>

struct Options {
	char* dev;
	char* name;
	char* udc;
	bool usage;
};

bool getopt_parse(int argc, char* argv[], struct Options*);
void getopt_free(struct Options*);
void usage(const char* arg0, bool help);
