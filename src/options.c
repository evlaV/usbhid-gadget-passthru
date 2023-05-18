/* SPDX-License-Identifier: BSD-3-Clause */
#include "log.h"
#include "options.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* default_name = "passthru";

bool getopt_parse(int argc, char* argv[], struct Options* opts) {
	static const char* flags = "hn:u:";
	static const struct option long_flags[] = {
		{"help", no_argument, 0, 'h'},
		{"name", required_argument, 0, 'n'},
		{"quiet", no_argument, 0, 'q'},
		{"udc", required_argument, 0, 'u'},
		{"verbose", no_argument, 0, 'v'},
		{0}
	};
	int c;
	opts->name = default_name;

	while ((c = getopt_long(argc, argv, flags, long_flags, NULL)) != -1) {
		switch (c) {
		case 'h':
			opts->usage = true;
			return true;
		case 'n':
			opts->name = strdup(optarg);
			break;
		case 'q':
			set_log_level(ERROR);
			break;
		case 'u':
			opts->udc = strdup(optarg);
			break;
		case 'v':
			set_log_level(DEBUG);
			break;
		default:
			return false;
		}
	}

	if (optind >= argc) {
		puts("Missing device name");
		return false;
	}
	opts->dev = strdup(argv[optind]);

	return true;
}

void getopt_free(struct Options* opts) {
	if (opts->dev) {
		free(opts->dev);
	}
	if (opts->name != default_name) {
		free(opts->name);
	}
	if (opts->udc) {
		free(opts->udc);
	}
}

void usage(const char* argv0, bool help) {
	if (help) {
		puts("USB HID device passthrough");
		puts("Copyright (c) 2022 Valve Software");
	}
	printf("Usage: %s [options] device\n", argv0);
	puts("\nOptions:");
	puts(" -h, --help         Print out this help");
	puts(" -n, --name NAME    Name of the passthru device, used in system paths");
	puts(" -q, --quiet        Print less output");
	puts(" -u, --udc UDC      Select which USB device controller to use for the gadget");
	puts(" -v, --verbose      Print more output");
	puts("\nThe device name may be either specified as a bus ID, as seen in "
	     "/sys/bus/usb/devices, or a VID:PID combination, in which case the first device "
	     "that matches that combination will be passed through.");
}
