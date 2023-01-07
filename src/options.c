/* SPDX-License-Identifier: BSD-3-Clause */
#include "log.h"
#include "options.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool getopt_parse(int argc, char* argv[], struct Options* opts) {
	static const char* flags = ":hn:qv";
	static const struct option long_flags[] = {
		{"help", no_argument, 0, 'h'},
		{"name", required_argument, 0, 'n'},
		{"quiet", no_argument, 0, 'q'},
		{"verbose", no_argument, 0, 'v'},
		{0}
	};
	int c;

	while ((c = getopt_long(argc, argv, flags, long_flags, NULL)) != -1) {
		switch (c) {
		case 'h':
			opts->usage = true;
			return true;
		case 'n':
			if (strchr(optarg, '/')) {
				log_fmt(ERROR, "Passthru name cannot include /\n");
				return false;
			}
			if (optarg[0] == '.') {
				log_fmt(ERROR, "Passthru name cannot start with .\n");
				return false;
			}
			opts->name = strdup(optarg);
			break;
		case 'q':
			set_log_level(ERROR);
			break;
		case 'v':
			set_log_level(DEBUG);
			break;
		default:
			if (opts->extra) {
				--optind;
				c = getopt_long(argc, argv, opts->extra->flags, opts->extra->options, NULL);
				if (opts->extra->parse(opts->extra->userdata, c)) {
					continue;
				}
			}
			return false;
		}
	}

	if (optind >= argc) {
		puts("Missing device name");
		return false;
	}
	if (strchr(argv[optind], '/')) {
		log_fmt(ERROR, "Device name cannot include /\n");
		return false;
	}
	if (argv[optind][0] == '.') {
		log_fmt(ERROR, "Device name cannot start with .\n");
		return false;
	}
	opts->dev = strdup(argv[optind]);

	return true;
}

void getopt_free(struct Options* opts) {
	if (opts->dev) {
		free(opts->dev);
	}
	if (opts->name) {
		free(opts->name);
	}
}

void usage(const char* argv0, bool help, struct OptionsExtra* extra) {
	if (help) {
		puts("USB HID device passthrough");
		puts("Copyright (c) 2022 Valve Software");
	}
	printf("Usage: %s [options] device\n", argv0);
	puts("\nOptions:");
	puts(" -h, --help         Print out this help");
	puts(" -n, --name NAME    Name of the passthru device, used in system paths");
	puts(" -q, --quiet        Print less output");
	puts(" -v, --verbose      Print more output");
	if (extra) {
		puts(extra->usage);
	}
	puts("\nThe device name may be either specified as a bus ID, as seen in "
	     "/sys/bus/usb/devices, or a VID:PID combination, in which case the first device "
	     "that matches that combination will be passed through.");
}
