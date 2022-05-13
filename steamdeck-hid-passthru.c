#include <errno.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <linux/input.h>
#include <linux/limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

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

bool cp_prop_function(const char* restrict indir, const char* inpath, const char* restrict outdir, const char* outpath, int fn) {
	char outtmp[PATH_MAX];
	snprintf(outtmp, sizeof(outtmp), outpath, fn);
	return cp_prop(indir, inpath, outdir, outtmp);
}

bool create_configfs(const char* configfs, const char* syspath, const struct hidraw_devinfo* devinfo) {
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

	if (!cp_prop(syspath, "../../bDeviceProtocol", configfs, "bDeviceProtocol")) {
		return false;
	}
	if (!cp_prop(syspath, "../../bDeviceSubClass", configfs, "bDeviceSubClass")) {
		return false;
	}
	if (!cp_prop(syspath, "../../manufacturer", configfs, "strings/0x409/manufacturer")) {
		return false;
	}
	if (!cp_prop(syspath, "../../product", configfs, "strings/0x409/product")) {
		return false;
	}
	if (!cp_prop(syspath, "../../serial", configfs, "strings/0x409/serialnumber")) {
		return false;
	}

	outfd = vopen("%s/idVendor", O_WRONLY, 0666, configfs);
	if (outfd < 0) {
		return false;
	}
	dprintf(outfd, "0x%04x\n", devinfo->vendor);
	close(outfd);

	outfd = vopen("%s/idProduct", O_WRONLY, 0666, configfs);
	if (outfd < 0) {
		return false;
	}
	dprintf(outfd, "0x%04x\n", devinfo->product);
	close(outfd);

	infd = vopen("%s/../../version", O_RDONLY, 0666, syspath);
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

bool create_configfs_function(const char* configfs, const char* syspath, const struct hidraw_report_descriptor* desc, int fn) {
	int outfd = -1;

	if (vmkdir("%s/functions/hid.usb%d", 0755, configfs, fn) == -1 && errno != -EEXIST) {
		return false;
	}

	if (!cp_prop_function(syspath, "../bInterfaceProtocol", configfs, "functions/hid.usb%d/protocol", fn)) {
		return false;
	}
	if (!cp_prop_function(syspath, "../bInterfaceSubClass", configfs, "functions/hid.usb%d/subclass", fn)) {
		return false;
	}

	outfd = vopen("%s/functions/hid.usb%d/report_length", O_WRONLY | O_TRUNC, 0666, configfs, fn);
	if (outfd < 0) {
		return false;
	}
	dprintf(outfd, "%02x", desc->size);
	close(outfd);

	outfd = vopen("%s/functions/hid.usb%d/report_desc", O_WRONLY | O_TRUNC, 0666, configfs, fn);
	if (outfd < 0) {
		return false;
	}
	if (write(outfd, desc->value, desc->size) != desc->size) {
		close(outfd);
		return false;
	}
	close(outfd);

	return true;
}


int main(int argc, char* argv[]) {
	int hidraw;
	struct hidraw_report_descriptor desc;
	struct hidraw_devinfo devinfo;
	char syspath[PATH_MAX];
	char syspath_tmp[PATH_MAX];
	char configfs[PATH_MAX];

	if (argc < 3) {
		return 0;
	}
	if (strncmp(argv[1], "/dev/hidraw", strlen("/dev/hidraw")) != 0) {
		return 1;
	}

	/* Resolve paths to sysfs nodes */
	snprintf(syspath_tmp, sizeof(syspath_tmp), "/sys/class/hidraw/%s", &argv[1][5]);
	if (realpath(syspath_tmp, syspath) == NULL) {
		return 1;
	}
	snprintf(syspath_tmp, sizeof(syspath_tmp), "%s/../..", syspath);
	if (realpath(syspath_tmp, syspath) == NULL) {
		return 1;
	}

	/* Get info about the hidraw and descriptors */
	hidraw = open(argv[1], O_RDONLY);
	if (hidraw < 0) {
		return 1;
	}
	if (ioctl(hidraw, HIDIOCGRDESCSIZE, &desc.size) == -1) {
		goto shutdown;
	}

	if (ioctl(hidraw, HIDIOCGRDESC, &desc) == -1) {
		goto shutdown;
	}

	if (ioctl(hidraw, HIDIOCGRAWINFO, &devinfo) == -1) {
		goto shutdown;
	}

	/* Create node using configfs */
	snprintf(configfs, sizeof(configfs), "/sys/kernel/config/usb_gadget/%s", argv[2]);
	if (!create_configfs(configfs, syspath, &devinfo)) {
		goto shutdown;
	}
	if (!create_configfs_function(configfs, syspath, &desc, 0)) {
		goto shutdown;
	}

	/* Drop privileges */
	/* TODO */
	printf("%s (%02x %04x:%04x) desc size %d\n", syspath, devinfo.bustype, devinfo.vendor, devinfo.product, desc.size);
	unsigned i;
	for (i = 0; i < desc.size; ++i) {
		printf("%02x", desc.value[i]);
		if ((i & 15) == 15) {
			putchar('\n');
		} else if ((i & 1) == 1) {
			putchar(' ');
		}
	}
	if ((desc.size & 15) != 15) {
		putchar('\n');
	}

shutdown:
	close(hidraw);
	return 0;
}
