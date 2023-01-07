/* SPDX-License-Identifier: BSD-3-Clause */
#include "dev.h"
#include "options.h"
#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <linux/hidraw.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define DESCRIPTOR_SIZE_MAX 4096
#define REPORT_SIZE_MAX 4096
#define INTERFACES_MAX 8

struct usb_hidg_report {
	uint16_t length;
	uint8_t data[64];
};

#define GADGET_HID_READ_SET_REPORT	_IOR('g', 0x41, struct usb_hidg_report)
#define GADGET_HID_WRITE_GET_REPORT	_IOW('g', 0x42, struct usb_hidg_report)

bool did_hup = false;

void hup() {
	did_hup = true;
}

bool create_configfs(const char* configfs, const char* syspath) {
	int outfd = -1;
	int infd = -1;
	char tmp[16];
	size_t i;

	if (mkdir(configfs, 0755) == -1 && errno != EEXIST) {
		perror("Failed to make configfs directory");
		return false;
	}

	if (vmkdir("%s/configs/c.1", 0755, configfs) == -1 && errno != EEXIST) {
		perror("Failed to make configfs configs directory");
		return false;
	}
	if (vmkdir("%s/strings/0x409", 0755, configfs) == -1 && errno != EEXIST) {
		perror("Failed to make configfs strings directory");
		return false;
	}
	if (vmkdir("%s/configs/c.1/strings/0x409", 0755, configfs) == -1 && errno != EEXIST) {
		perror("Failed to make configfs configs strings directory");
		return false;
	}

	if (!cp_prop(syspath, "bDeviceProtocol", configfs, "bDeviceProtocol")) {
		return false;
	}
	if (!cp_prop(syspath, "bDeviceSubClass", configfs, "bDeviceSubClass")) {
		return false;
	}
	if (!cp_prop(syspath, "manufacturer", configfs, "strings/0x409/manufacturer")) {
		return false;
	}
	if (!cp_prop(syspath, "product", configfs, "strings/0x409/product")) {
		return false;
	}
	if (!cp_prop(syspath, "serial", configfs, "strings/0x409/serialnumber")) {
		return false;
	}
	if (!cp_prop(syspath, "configuration", configfs, "configs/c.1/strings/0x409/configuration")) {
		return false;
	}

	if (!cp_prop_hex(syspath, "idVendor", configfs, "idVendor")) {
		return false;
	}
	if (!cp_prop_hex(syspath, "idProduct", configfs, "idProduct")) {
		return false;
	}
	if (!cp_prop_hex(syspath, "bcdDevice", configfs, "bcdDevice")) {
		return false;
	}

	infd = vopen("%s/version", O_RDONLY, 0666, syspath);
	if (infd < 0) {
		perror("Failed to open version input file");
		return false;
	}
	if (read(infd, tmp, sizeof(tmp)) < 0) {
		perror("Failed to read version file");
		close(infd);
		return false;
	}
	close(infd);
	/* Convert from human readable to BCD: */
	/* s/^ (.)\.(...)/0x0\1\2/m */
	tmp[6] = tmp[5];
	tmp[5] = tmp[4];
	tmp[4] = tmp[3];
	tmp[3] = tmp[1];
	tmp[0] = '0';
	tmp[1] = 'x';
	tmp[2] = '0';
	outfd = vopen("%s/bcdUSB", O_WRONLY, 0666, configfs);
	if (outfd < 0) {
		perror("Failed to open version output file");
		return false;
	}
	if (write(outfd, tmp, 7) != 7) {
		perror("Failed to write version output file");
		close(outfd);
		return false;
	}
	close(outfd);

	infd = vopen("%s/bMaxPower", O_RDONLY, 0666, syspath);
	if (infd < 0) {
		perror("Failed to open max power input file");
		return false;
	}
	if (read(infd, tmp, sizeof(tmp)) < 0) {
		perror("Failed to read max power file");
		close(infd);
		return false;
	}
	close(infd);
	/* Chop off units */
	for (i = 0; i < sizeof(tmp) - 1; ++i) {
		if (isdigit(tmp[i])) {
			continue;
		}
		if (tmp[i] == 'm') {
			tmp[i] = '\n';
			tmp[i + 1] = '\0';
			break;
		}
		return false;
	}
	if (i == sizeof(tmp) - 1) {
		return false;
	}
	outfd = vopen("%s/configs/c.1/MaxPower", O_WRONLY, 0666, configfs);
	if (outfd < 0) {
		perror("Failed to open max power output file");
		return false;
	}
	if (write(outfd, tmp, strlen(tmp)) < 0) {
		perror("Failed to write max power output file");
		close(outfd);
		return false;
	}
	close(outfd);

	return true;
}

bool create_configfs_function(const char* configfs, const char* syspath, int fn) {
	char function[PATH_MAX];
	char interface[PATH_MAX];
	char report_descriptor[DESCRIPTOR_SIZE_MAX];
	int infd;
	int outfd = -1;
	ssize_t desc_size;

	snprintf(function, sizeof(function), "%s/functions/hid.usb%d", configfs, fn);
	if (mkdir(function, 0755) == -1 && errno != EEXIST) {
		perror("Failed to make configfs function directory");
		return false;
	}

	if (!cp_prop(syspath, "bInterfaceProtocol", function, "protocol")) {
		return false;
	}
	if (!cp_prop(syspath, "bInterfaceSubClass", function, "subclass")) {
		return false;
	}

	if (!find_function(syspath, interface, sizeof(interface))) {
		puts("Failed to find function");
		return false;
	}
	infd = vopen("%s/report_descriptor", O_RDONLY, 0666, interface);
	if (infd < 0) {
		perror("Failed to open report descriptor input file");
		return false;
	}

	desc_size = read(infd, report_descriptor, sizeof(report_descriptor));
	if (desc_size <= 0) {
		perror("Failed to read report descriptor file");
		return false;
	}

	outfd = vopen("%s/report_desc", O_WRONLY | O_TRUNC, 0666, function);
	if (outfd < 0) {
		perror("Failed to open report descriptor output file");
		return false;
	}

	if (write(outfd, report_descriptor, desc_size) != desc_size) {
		perror("Failed to write report descriptor file");
		close(outfd);
		return false;
	}
	close(outfd);


	outfd = vopen("%s/report_length", O_WRONLY | O_TRUNC, 0666, function);
	if (outfd < 0) {
		perror("Failed to open report length file");
		return false;
	}
	if (dprintf(outfd, "%02i", 64) < 2) {
		perror("Failed to write report length file");
		close(outfd);
		return false;
	}
	close(outfd);

	snprintf(interface, sizeof(interface), "%s/configs/c.1/hid.usb%d", configfs, fn);
	if (symlink(function, interface) < 0) {
		perror("Failed to symlink interface config");
		return false;
	}

	return true;
}

bool find_udc(char* out) {
	DIR* dir;
	struct dirent* dent;

	dir = opendir("/sys/class/udc");
	if (!dir) {
		perror("Failed to opendir udc");
		return false;
	}

	while ((dent = readdir(dir))) {
		if (dent->d_name[0] == '.') {
			continue;
		}
		strncpy(out, dent->d_name, PATH_MAX - 1);
		break;
	}
	closedir(dir);
	return !!dent;
}

bool start_udc(const char* configfs, const char* udc) {
	int fd = vopen("%s/UDC", O_WRONLY | O_TRUNC, 0644, configfs);
	if (fd < 0) {
		perror("Failed to open UDC");
		return false;
	}
	if (dprintf(fd, "%s\n", udc) < 0) {
		perror("Failed to start UDC");
		close(fd);
		return false;
	}
	close(fd);
	return true;
}

bool stop_udc(const char* configfs) {
	int fd = vopen("%s/UDC", O_WRONLY | O_TRUNC, 0644, configfs);
	if (fd < 0) {
		perror("Failed to open UDC");
		return false;
	}
	if (write(fd, "\n", 1) < 0) {
		perror("Failed to stop UDC");
		close(fd);
		return false;
	}
	close(fd);
	return true;
}

bool poll_fds(int* infds, int* outfds, nfds_t nfds) {
	struct pollfd fds[INTERFACES_MAX * 2];
	struct pollfd outpoll;
	uint8_t buffer[REPORT_SIZE_MAX];
	ssize_t sizein;
	ssize_t sizeout;
	ssize_t loc;
	struct usb_hidg_report set_report;
	struct usb_hidg_report get_report = { 64, { 0 } };
	nfds_t i;

	for (i = 0; i < nfds; ++i) {
		fds[i * 2].fd = infds[i];
		fds[i * 2].events = POLLIN;
		fds[i * 2 + 1].fd = outfds[i];
		fds[i * 2 + 1].events = POLLIN | POLLPRI;
	}

	while (!did_hup) {
		int ret = poll(fds, nfds * 2, -1);
		if (ret == -EAGAIN) {
			continue;
		}
		if (ret < 0) {
			if (errno != EINTR) {
				perror("Failed to poll nodes");
			}
			return did_hup;
		}
		for (i = 0; i < nfds * 2; ++i) {
			if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				return did_hup;
			}
			if (fds[i].revents & POLLPRI) {
				if (ioctl(fds[i].fd, GADGET_HID_READ_SET_REPORT, &set_report) < 0) {
					perror("SET ioctl in failed");
				}
				if (ioctl(fds[i ^ 1].fd, HIDIOCSFEATURE(set_report.length), set_report.data) < 0) {
					perror("SET ioctl out failed");
				}
				get_report.data[0] = set_report.data[0];
				if (ioctl(fds[i ^ 1].fd, HIDIOCGFEATURE(64), get_report.data) < 0) {
					perror("GET ioctl in failed");
				}
				if (get_report.data[0] == set_report.data[0] && ioctl(fds[i].fd, GADGET_HID_WRITE_GET_REPORT, &get_report) < 0) {
					perror("GET ioctl out failed");
				}
				memset(get_report.data, 0, sizeof(get_report.data));
				fds[i].revents &= ~POLLPRI;
			}
			if (fds[i].revents & POLLIN) {
				outpoll.fd = fds[i ^ 1].fd;
				outpoll.events = POLLOUT;
				outpoll.revents = 0;
				if (poll(&outpoll, 1, 0) != 1) {
					continue;
				}

				sizein = read(fds[i].fd, buffer, sizeof(buffer));
				if (sizein < 0) {
					if (errno != EINTR) {
						perror("Failed to read packet");
					}
					return did_hup;
				}
				loc = 0;
				while (sizein > 0) {
					sizeout = write(fds[i ^ 1].fd, &buffer[loc], sizein);
					if (sizeout < 0) {
						if (errno == EAGAIN) {
							break;
						}
						if (errno != EINTR) {
							perror("Failed to write packet");
						}
						return did_hup;
					}
					loc += sizeout;
					sizein -= sizeout;
				}
				fds[i].revents &= ~POLLIN;
			}
		}
	}
	return true;
}

int main(int argc, char* argv[]) {
	char syspath[PATH_MAX];
	char syspath_tmp[PATH_MAX];
	char configfs[PATH_MAX];
	char udc[PATH_MAX];
	char bus_id[32];
	char tmp[16];
	int fd;
	int hidg[INTERFACES_MAX];
	int hidraw[INTERFACES_MAX];
	bool is_hid[INTERFACES_MAX];
	int ret;
	unsigned max_interfaces = 0;
	unsigned i, j;
	struct sigaction sa;
	struct Options opts = {0};
	int ok = 1;

	if (!getopt_parse(argc, argv, &opts)) {
		usage(argv[0], false);
		goto shutdown;
	}
	if (opts.usage) {
		usage(argv[0], true);
		getopt_free(&opts);
		return 0;
	}

	/* Resolve paths to sysfs nodes */
	if (strchr(opts.dev, ':') && strlen(opts.dev) == 9) {
		find_dev_by_id(opts.dev, syspath_tmp);
	} else {
		snprintf(syspath_tmp, sizeof(syspath_tmp), "/sys/bus/usb/devices/%s", opts.dev);
	}
	if (realpath(syspath_tmp, syspath) == NULL) {
		perror("Failed to resolve sysfs path");
		goto shutdown;
	}
	strncpy(bus_id, strrchr(syspath_tmp, '/') + 1, sizeof(bus_id) - 1);

	/* Create node using configfs */
	fd = vopen("%s/bNumInterfaces", O_RDONLY, 0666, syspath);
	if (fd < 0) {
		perror("Failed to open interface count");
		goto shutdown;
	}
	if (read(fd, tmp, sizeof(tmp)) < 0) {
		perror("Failed to read interface count");
		goto shutdown;
	}
	max_interfaces = strtoul(tmp, NULL, 10);
	if (max_interfaces > INTERFACES_MAX) {
		max_interfaces = INTERFACES_MAX;
	}

	/* We want to exit cleanly in event of SIGINT or SIGHUP */
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = hup;
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	snprintf(configfs, sizeof(configfs), "/sys/kernel/config/usb_gadget/%s", opts.name);
	if (!create_configfs(configfs, syspath)) {
		goto shutdown;
	}

	for (i = 0; i < max_interfaces; ++i) {
		snprintf(syspath_tmp, sizeof(syspath_tmp), "%s/%s:1.%u", syspath, bus_id, i);
		fd = vopen("%s/bInterfaceClass", O_RDONLY, 0666, syspath_tmp);
		if (fd < 0) {
			perror("Could not determine interface class");
			goto shutdown;
		}
		if (read(fd, tmp, 3) != 3) {
			perror("Could not determine interface class");
			close(fd);
			goto shutdown;
		}
		close(fd);
		is_hid[i] = memcmp(tmp, "03\n", 3) == 0;
		if (!is_hid[i]) {
			continue;
		}

		if (!create_configfs_function(configfs, syspath_tmp, i)) {
			perror("Could not create function");
			goto shutdown;
		}
	}

	if (opts.udc) {
		strncpy(udc, opts.udc, sizeof(udc) - 1);
	} else if (!find_udc(udc)) {
		perror("Could not find UDC");
		goto shutdown;
	}

	if (!start_udc(configfs, udc)) {
		goto shutdown;
	}

	for (i = 0, j = 0; i < max_interfaces; ++i) {
		if (!is_hid[i]) {
			hidg[j] = -1;
			hidraw[j] = -1;
			continue;
		}
		snprintf(syspath_tmp, sizeof(syspath_tmp), "%s/functions/hid.usb%u/dev", configfs, i);
		hidg[j] = find_dev(syspath_tmp, "hidg");
		ret = fcntl(hidg[j], F_GETFL, 0);
		if (ret < 0) {
			perror("Failed to get dev flags");
			goto late_shutdown;
		}
		fcntl(hidg[j], F_SETFL, ret | FNONBLOCK);
		snprintf(syspath_tmp, sizeof(syspath_tmp), "%s/%s:1.%u", syspath, bus_id, i);
		hidraw[j] = find_hidraw(syspath_tmp);
		if (hidg[j] < 0 || hidraw[j] < 0) {
			for (i = 0; i < max_interfaces; ++i) {
				if (hidg[i] >= 0) {
					close(hidg[i]);
				}
				if (hidraw[i] >= 0) {
					close(hidraw[i]);
				}
			}
			goto late_shutdown;
		}
		++j;
	}
	max_interfaces = j;

	if (did_hup) {
		goto late_shutdown;
	}

	ok = !poll_fds(hidraw, hidg, max_interfaces);

	for (i = 0; i < max_interfaces; ++i) {
		close(hidg[i]);
		close(hidraw[i]);
	}

late_shutdown:
	stop_udc(configfs);
shutdown:
	snprintf(syspath_tmp, sizeof(syspath_tmp), "%s/strings/0x409", configfs);
	rmdir(syspath_tmp);
	snprintf(syspath_tmp, sizeof(syspath_tmp), "%s/configs/c.1/strings/0x409", configfs);
	rmdir(syspath_tmp);
	for (i = 0; i < max_interfaces; ++i) {
		snprintf(syspath_tmp, sizeof(syspath_tmp), "%s/configs/c.1/hid.usb%u", configfs, i);
		unlink(syspath_tmp);
		snprintf(syspath_tmp, sizeof(syspath_tmp), "%s/functions/hid.usb%u", configfs, i);
		rmdir(syspath_tmp);
	}
	snprintf(syspath_tmp, sizeof(syspath_tmp), "%s/configs/c.1", configfs);
	rmdir(syspath_tmp);
	rmdir(configfs);
	getopt_free(&opts);
	return ok;
}
