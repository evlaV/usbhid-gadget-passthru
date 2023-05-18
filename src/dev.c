/* SPDX-License-Identifier: BSD-3-Clause */
#include "dev.h"
#include "log.h"
#include "util.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

bool find_function(const char* syspath, char* function, size_t function_size) {
	DIR* dir;
	struct dirent* dent;
	dir = opendir(syspath);
	if (!dir) {
		log_errno(ERROR, "Failed to opendir function");
		return false;
	}

	while ((dent = readdir(dir))) {
		if (dent->d_type != DT_DIR) {
			continue;
		}
		if (strncmp(dent->d_name, "0003:", 5) == 0) {
			snprintf(function, function_size, "%s/%s", syspath, dent->d_name);
			break;
		}
	}
	closedir(dir);
	return !!dent;
}

int find_dev_node(unsigned nod_major, unsigned nod_minor, const char* prefix) {
	char nod_path[PATH_MAX];
	DIR* dir;
	struct dirent* dent;
	struct stat nod;
	dir = opendir("/dev");
	if (!dir) {
		log_errno(ERROR, "Failed to opendir /dev");
		return -1;
	}

	while ((dent = readdir(dir))) {
		if (dent->d_type != DT_CHR) {
			continue;
		}
		if (strncmp(dent->d_name, prefix, strlen(prefix)) != 0) {
			continue;
		}
		snprintf(nod_path, sizeof(nod_path), "/dev/%s", dent->d_name);
		if (stat(nod_path, &nod) < 0) {
			log_errno(ERROR, "Failed to stat dev node");
			return -1;
		}
		if (major(nod.st_rdev) == nod_major && minor(nod.st_rdev) == nod_minor) {
			closedir(dir);
			return open(nod_path, O_RDWR, 0666);
		}
	}
	closedir(dir);
	return -1;
}

int find_dev(const char* file, const char* class) {
	char tmp[16];
	char* parse_tmp;
	unsigned nod_major;
	unsigned nod_minor;

	int fd = open(file, O_RDONLY);
	if (fd < 0) {
		log_errno(ERROR, "Failed to open dev path");
		return -1;
	}
	if (read(fd, tmp, sizeof(tmp)) < 3) {
		log_errno(ERROR, "Failed to read dev path");
		close(fd);
		return -1;
	}
	close(fd);
	nod_major = strtoul(tmp, &parse_tmp, 10);
	if (!parse_tmp || parse_tmp[0] != ':') {
		return -1;
	}
	nod_minor = strtoul(&parse_tmp[1], NULL, 10);
	if (!parse_tmp) {
		return -1;
	}
	return find_dev_node(nod_major, nod_minor, class);
}

bool find_dev_by_id(const char* vidpid, char* out) {
	DIR* dir;
	struct dirent* dent;
	char tmp[5] = {0};
	int fd;

	dir = opendir("/sys/bus/usb/devices");
	if (!dir) {
		log_errno(ERROR, "Failed to opendir usb/devices");
		return false;
	}

	while ((dent = readdir(dir))) {
		if (strncmp(dent->d_name, "usb", 3) == 0) {
			continue;
		}
		if (dent->d_name[0] == '.') {
			continue;
		}
		if (strchr(dent->d_name, ':')) {
			continue;
		}

		fd = vopen("/sys/bus/usb/devices/%s/idVendor", O_RDONLY, 0666, dent->d_name);
		if (fd < 0) {
			continue;
		}
		if (read(fd, tmp, 4) < 4) {
			close(fd);
			continue;
		}
		close(fd);
		if (strncasecmp(tmp, vidpid, 4) != 0) {
			continue;
		}

		fd = vopen("/sys/bus/usb/devices/%s/idProduct", O_RDONLY, 0666, dent->d_name);
		if (fd < 0) {
			continue;
		}
		if (read(fd, tmp, 4) < 4) {
			close(fd);
			continue;
		}
		close(fd);
		if (strncasecmp(tmp, &vidpid[5], 4) != 0) {
			continue;
		}
		snprintf(out, PATH_MAX, "/sys/bus/usb/devices/%s", dent->d_name);
		break;
	}
	closedir(dir);
	return !!dent;
}

int find_hidraw(const char* syspath) {
	char function[PATH_MAX];
	char filename[PATH_MAX];
	DIR* dir;
	struct dirent* dent;

	if (!find_function(syspath, function, sizeof(function))) {
		return -1;
	}
	strncat(function, "/hidraw", sizeof(function));
	dir = opendir(function);
	if (!dir) {
		log_errno(ERROR, "Failed to opendir hidraw");
		return -1;
	}

	while ((dent = readdir(dir))) {
		if (dent->d_type != DT_DIR) {
			continue;
		}
		if (strncmp(dent->d_name, "hidraw", 6) == 0) {
			snprintf(filename, sizeof(filename), "%s/%s/dev", function, dent->d_name);
			break;
		}
	}
	if (!dent) {
		closedir(dir);
		return -1;
	}
	closedir(dir);
	return find_dev(filename, "hidraw");
}
