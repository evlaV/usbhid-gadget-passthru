#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/input.h>
#include <linux/limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define DESCRIPTOR_SIZE_MAX 4096

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

	if (mkdir(configfs, 0755) == -1 && errno != -EEXIST) {
		return false;
	}

	if (vmkdir("%s/configs/c.1", 0755, configfs) == -1 && errno != -EEXIST) {
		return false;
	}
	if (vmkdir("%s/strings/0x409", 0755, configfs) == -1 && errno != -EEXIST) {
		return false;
	}
	if (vmkdir("%s/configs/c.1/strings/0x409", 0755, configfs) == -1 && errno != -EEXIST) {
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

	outfd = vopen("%s/configs/c.1/strings/0x409/configuration", O_WRONLY | O_TRUNC, 0666, configfs);
	if (outfd < 0) {
		return false;
	}
	dprintf(outfd, "Configuration 1\n");
	close(outfd);

	return true;
}

bool create_configfs_function(const char* configfs, const char* syspath, int fn) {
	char function[PATH_MAX];
	char interface[PATH_MAX];
	char report_descriptor[DESCRIPTOR_SIZE_MAX];
	int infd;
	int outfd = -1;
	DIR* dir;
	struct dirent* dent;
	ssize_t desc_size;

	snprintf(function, sizeof(function), "%s/functions/hid.usb%d", configfs, fn);
	if (mkdir(function, 0755) == -1 && errno != -EEXIST) {
		return false;
	}

	if (!cp_prop(syspath, "bInterfaceProtocol", function, "protocol")) {
		return false;
	}
	if (!cp_prop(syspath, "bInterfaceSubClass", function, "subclass")) {
		return false;
	}

	dir = opendir(syspath);
	if (!dir) {
		return false;
	}

	while ((dent = readdir(dir))) {
		if (dent->d_type != DT_DIR) {
			continue;
		}
		if (strncmp(dent->d_name, "0003:", 5) == 0) {
			snprintf(interface, sizeof(interface), "%s/%s", syspath, dent->d_name);
			break;
		}
	}
	closedir(dir);
	if (!dent) {
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
	dprintf(outfd, "%02" PRIxPTR, desc_size);
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


int main(int argc, char* argv[]) {
	char syspath[PATH_MAX];
	char syspath_tmp[PATH_MAX];
	char configfs[PATH_MAX];
	char tmp[16];
	const char* bus_id;
	int fd;
	unsigned max_interfaces = 0;
	unsigned i;

	if (argc < 3) {
		return 0;
	}
	bus_id = argv[1];

	/* Resolve paths to sysfs nodes */
	snprintf(syspath_tmp, sizeof(syspath_tmp), "/sys/bus/usb/devices/%s", bus_id);
	if (realpath(syspath_tmp, syspath) == NULL) {
		return 1;
	}

	/* Create node using configfs */
	snprintf(configfs, sizeof(configfs), "/sys/kernel/config/usb_gadget/%s", argv[2]);
	if (!create_configfs(configfs, syspath)) {
		goto shutdown;
	}

	fd = vopen("%s/bNumInterfaces", O_RDONLY, 0666, syspath);
	if (fd < 0) {
		goto shutdown;
	}
	if (read(fd, tmp, sizeof(tmp)) < 0) {
		goto shutdown;
	}
	max_interfaces = strtoul(tmp, NULL, 10);
	for (i = 0; i < max_interfaces; ++i) {
		snprintf(syspath_tmp, sizeof(syspath_tmp), "%s/%s:1.%u", syspath, bus_id, i);
		if (!create_configfs_function(configfs, syspath_tmp, i)) {
			goto shutdown;
		}
	}

	/* Drop privileges */
	/* TODO */
	return 0;

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
	return 1;
}
