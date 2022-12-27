/* SPDX-License-Identifier: BSD-3-Clause */
#include "dev.h"
#include "util.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fcntl.h>
#include <unistd.h>

bool find_sysfs_path(const char* name, char* syspath, char* bus_id) {
	char syspath_tmp[PATH_MAX];

	if (strchr(name, ':') && strlen(name) == 9) {
		find_dev_by_id(name, syspath_tmp);
	} else {
		snprintf(syspath_tmp, sizeof(syspath_tmp), "/sys/bus/usb/devices/%s", name);
	}
	if (realpath(syspath_tmp, syspath) == NULL) {
		perror("Failed to resolve sysfs path");
		return false;
	}
	strncpy(bus_id, strrchr(syspath_tmp, '/') + 1, 15);
	return true;
}

int interface_count(const char* syspath) {
	char tmp[16];
	int fd;

	fd = vopen("%s/bNumInterfaces", O_RDONLY, 0666, syspath);
	if (fd < 0) {
		perror("Failed to open interface count");
		return -1;
	}
	if (read(fd, tmp, sizeof(tmp)) < 0) {
		perror("Failed to read interface count");
		return -1;
	}
	return strtoul(tmp, NULL, 10);
}

int interface_type(const char* syspath, const char* bus_id, int interface) {
	char syspath_tmp[PATH_MAX];
	char tmp[3];
	int fd;
	snprintf(syspath_tmp, sizeof(syspath_tmp), "%s/%s:1.%u", syspath, bus_id, interface);
	fd = vopen("%s/bInterfaceClass", O_RDONLY, 0666, syspath_tmp);
	if (fd < 0) {
		perror("Could not determine interface class");
		return fd;
	}
	if (read(fd, tmp, 3) != 3) {
		perror("Could not determine interface class");
		close(fd);
		return -1;
	}
	close(fd);
	return strtoul(tmp, NULL, 16);
}
