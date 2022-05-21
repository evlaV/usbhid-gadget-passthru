#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define DESCRIPTOR_SIZE_MAX 4096
#define REPORT_SIZE_MAX 4096
#define INTERFACES_MAX 16

bool did_hup = false;

void hup() {
	did_hup = true;
}

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
		close(outfd);
		return false;
	}

	if (outfd < 0) {
		close(infd);
		return false;
	}

	while ((size = read(infd, buf, sizeof(buf))) > 0) {
		if (write(outfd, buf, size) != size) {
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
		close(outfd);
		return false;
	}

	if (outfd < 0) {
		close(infd);
		return false;
	}

	buf[0] = '0';
	buf[1] = 'x';
	size = read(infd, &buf[2], sizeof(buf) - 2);
	if (size < 1) {
		close(infd);
		close(outfd);
		return false;
	}
	size += 2;

	if (write(outfd, buf, size) != size) {
		close(infd);
		close(outfd);
		return false;
	}

	close(infd);
	close(outfd);
	return true;
}

bool create_configfs(const char* configfs, const char* syspath) {
	int outfd = -1;
	int infd = -1;
	char tmp[16];
	size_t i;

	if (mkdir(configfs, 0755) == -1 && errno != EEXIST) {
		return false;
	}

	if (vmkdir("%s/configs/c.1", 0755, configfs) == -1 && errno != EEXIST) {
		return false;
	}
	if (vmkdir("%s/strings/0x409", 0755, configfs) == -1 && errno != EEXIST) {
		return false;
	}
	if (vmkdir("%s/configs/c.1/strings/0x409", 0755, configfs) == -1 && errno != EEXIST) {
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
		return false;
	}
	if (read(infd, tmp, sizeof(tmp)) < 0) {
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
		return false;
	}
	write(outfd, tmp, 7);
	close(outfd);

	infd = vopen("%s/bMaxPower", O_RDONLY, 0666, syspath);
	if (infd < 0) {
		return false;
	}
	if (read(infd, tmp, sizeof(tmp)) < 0) {
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
		return false;
	}
	write(outfd, tmp, strlen(tmp));
	close(outfd);

	return true;
}

bool find_function(const char* syspath, char* function, size_t function_size) {
	DIR* dir;
	struct dirent* dent;
	dir = opendir(syspath);
	if (!dir) {
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

bool create_configfs_function(const char* configfs, const char* syspath, int fn) {
	char function[PATH_MAX];
	char interface[PATH_MAX];
	char report_descriptor[DESCRIPTOR_SIZE_MAX];
	int infd;
	int outfd = -1;
	ssize_t desc_size;

	snprintf(function, sizeof(function), "%s/functions/hid.usb%d", configfs, fn);
	if (mkdir(function, 0755) == -1 && errno != EEXIST) {
		return false;
	}

	if (!cp_prop(syspath, "bInterfaceProtocol", function, "protocol")) {
		return false;
	}
	if (!cp_prop(syspath, "bInterfaceSubClass", function, "subclass")) {
		return false;
	}

	if (!find_function(syspath, interface, sizeof(interface))) {
		return false;
	}
	infd = vopen("%s/report_descriptor", O_RDONLY, 0666, interface);
	if (infd < 0) {
		return false;
	}

	desc_size = read(infd, report_descriptor, sizeof(report_descriptor));
	if (desc_size <= 0) {
		return false;
	}

	outfd = vopen("%s/report_length", O_WRONLY | O_TRUNC, 0666, function);
	if (outfd < 0) {
		return false;
	}
	dprintf(outfd, "%02i", 64); // TODO
	close(outfd);

	outfd = vopen("%s/report_desc", O_WRONLY | O_TRUNC, 0666, function);
	if (outfd < 0) {
		return false;
	}
	if (write(outfd, report_descriptor, desc_size) != desc_size) {
		close(outfd);
		return false;
	}
	close(outfd);

	snprintf(interface, sizeof(interface), "%s/configs/c.1/hid.usb%d", configfs, fn);
	symlink(function, interface);

	return true;
}

int find_dev_node(unsigned nod_major, unsigned nod_minor, const char* prefix) {
	char nod_path[PATH_MAX];
	DIR* dir;
	struct dirent* dent;
	struct stat nod;
	dir = opendir("/dev");
	if (!dir) {
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
		return -1;
	}
	if (read(fd, tmp, sizeof(tmp)) < 3) {
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

bool start_udc(const char* configfs, const char* udc) {
	int fd = vopen("%s/UDC", O_WRONLY | O_TRUNC, 0644, configfs);
	if (fd < 0) {
		return false;
	}
	dprintf(fd, "%s\n", udc);
	close(fd);
	return true;
}

bool stop_udc(const char* configfs) {
	int fd = vopen("%s/UDC", O_WRONLY | O_TRUNC, 0644, configfs);
	if (fd < 0) {
		return false;
	}
	write(fd, "\n", 1);
	close(fd);
	return true;
}

bool poll_fds(int* infds, int* outfds, nfds_t nfds) {
	struct pollfd fds[INTERFACES_MAX * 2];
	uint8_t buffer[REPORT_SIZE_MAX];
	ssize_t sizein;
	ssize_t sizeout;
	ssize_t loc;
	nfds_t i;

	for (i = 0; i < nfds; ++i) {
		fds[i * 2].fd = infds[i];
		fds[i * 2].events = POLLIN;
		fds[i * 2 + 1].fd = outfds[i];
		fds[i * 2 + 1].events = POLLIN;
	}

	while (true) {
		int ret = poll(fds, nfds * 2, -1);
		if (ret == -EAGAIN) {
			continue;
		}
		if (ret < 0) {
			return did_hup;
		}
		for (i = 0; i < nfds * 2; ++i) {
			if (fds[i].revents & POLLIN) {
				sizein = read(fds[i].fd, buffer, sizeof(buffer));
				if (sizein < 0) {
					return did_hup;
				}
				loc = 0;
				while (sizein > 0) {
					sizeout = write(fds[i ^ 1].fd, &buffer[loc], sizein);
					if (sizeout < 0) {
						return did_hup;
					}
					loc += sizeout;
					sizein -= sizeout;
				}
				fds[i].revents &= ~POLLIN;
			}
			if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				return did_hup;
			}
		}
	}
}

int main(int argc, char* argv[]) {
	char syspath[PATH_MAX];
	char syspath_tmp[PATH_MAX];
	char configfs[PATH_MAX];
	char tmp[16];
	const char* bus_id;
	int fd;
	int hidg[INTERFACES_MAX];
	int hidraw[INTERFACES_MAX];
	unsigned max_interfaces = 0;
	unsigned i;
	struct sigaction sa;
	int ok = 1;

	if (argc != 4) {
		return 0;
	}
	bus_id = argv[1];

	/* Resolve paths to sysfs nodes */
	snprintf(syspath_tmp, sizeof(syspath_tmp), "/sys/bus/usb/devices/%s", bus_id);
	if (realpath(syspath_tmp, syspath) == NULL) {
		return 1;
	}

	/* Create node using configfs */
	fd = vopen("%s/bNumInterfaces", O_RDONLY, 0666, syspath);
	if (fd < 0) {
		return 1;
	}
	if (read(fd, tmp, sizeof(tmp)) < 0) {
		return 1;
	}
	max_interfaces = strtoul(tmp, NULL, 10);

	/* We want to exit cleanly in event of SIGINT or SIGHUP */
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = hup;
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	snprintf(configfs, sizeof(configfs), "/sys/kernel/config/usb_gadget/%s", argv[2]);
	if (!create_configfs(configfs, syspath)) {
		goto shutdown;
	}

	for (i = 0; i < max_interfaces; ++i) {
		snprintf(syspath_tmp, sizeof(syspath_tmp), "%s/%s:1.%u", syspath, bus_id, i);
		if (!create_configfs_function(configfs, syspath_tmp, i)) {
			goto shutdown;
		}
	}

	if (!start_udc(configfs, argv[3])) {
		goto shutdown;
	}

	for (i = 0; i < max_interfaces && i < INTERFACES_MAX; ++i) {
		snprintf(syspath_tmp, sizeof(syspath_tmp), "%s/functions/hid.usb%u/dev", configfs, i);
		hidg[i] = find_dev(syspath_tmp, "hidg");
		snprintf(syspath_tmp, sizeof(syspath_tmp), "%s/%s:1.%u", syspath, bus_id, i);
		hidraw[i] = find_hidraw(syspath_tmp);
		if (hidg[i] < 0 || hidraw[i] < 0) {
			if (i + 1 < INTERFACES_MAX) {
				hidg[i + 1] = -1;
				hidraw[i + 1] = -1;
			}
			for (i = 0; hidg[i] != -1; ++i) {
				close(hidg[i]);
			}
			for (i = 0; hidraw[i] != -1; ++i) {
				close(hidraw[i]);
			}
			goto shutdown;
		}
	}

	if (did_hup) {
		goto shutdown;
	}

	ok = !poll_fds(hidraw, hidg, i);

	for (i = 0; i < max_interfaces && i < INTERFACES_MAX; ++i) {
		close(hidg[i]);
		close(hidraw[i]);
	}

shutdown:
	stop_udc(configfs);
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
	return ok;
}
