/* SPDX-License-Identifier: BSD-3-Clause */
#include "dbus.h"
#include "dev.h"
#include "filter.h"
#include "gatt.h"
#include "log.h"
#include "options.h"
#include "usb.h"
#include "util.h"

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/l2cap.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <linux/hidraw.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/fcntl.h>
#include <unistd.h>

#define UUID16(U) #U

/* Services */
#define UUID_DEV_INFO  UUID16(180a)
#define UUID_BATTERY   UUID16(180f)
#define UUID_HID       UUID16(1812)

/* Descriptors */
#define UUID_REPORT_REFERENCE UUID16(2908)

/* Characteristics */
#define UUID_BATTERY_LEVEL      UUID16(2a19)
#define UUID_HID_INFO           UUID16(2a4a)
#define UUID_REPORT_MAP         UUID16(2a4b)
#define UUID_HID_CONTROL        UUID16(2a4c)
#define UUID_REPORT             UUID16(2a4d)
#define UUID_PNP_ID             UUID16(2a50)

#define GAP_GAMEPAD 0x03C4

#define REPORT_TYPE_INPUT 1
#define REPORT_TYPE_OUTPUT 2
#define REPORT_TYPE_FEATURE 3

#define DESCRIPTOR_SIZE_MAX 4096
#define REPORT_SIZE_MAX 512
#define INTERFACES_MAX 8
#define FLUSH_INTERVAL 250000000ULL
#define FLUSH_THROTTLE  20000000ULL

bool did_hup = false;
bool did_error = false;

void hup() {
	did_hup = true;
}

struct LEAdvertisement {
	const char* type;
	const char** uuids;
	const char* local_name;
	uint16_t appearance;
	uint16_t duration;
	uint16_t timeout;
};

struct PnPID {
	uint8_t source;
	uint16_t vid;
	uint16_t pid;
	uint16_t version;
} __attribute__((packed));

struct HIDInfo {
	uint16_t bcdHID;
	uint8_t bCountryCode;
	uint8_t flags;
};

struct ReportReference {
	uint8_t reportId;
	uint8_t reportType;
};

struct HOGPInterface {
	int id;
	struct GattService hid;

	struct GattCharacteristic hid_info;
	struct GattCharacteristic report_map;
	struct GattCharacteristic hid_control;
	struct GattCharacteristic input_report;
	struct GattCharacteristic output_report;
	struct GattCharacteristic feature_report;

	struct GattDescriptor input_report_reference;
	struct GattDescriptor input_report_client_config;
	struct GattDescriptor output_report_reference;
	struct GattDescriptor feature_report_reference;

	struct HIDInfo hid_info_data;

	struct ReportReference input_report_reference_data;
	struct ReportReference output_report_reference_data;
	struct ReportReference feature_report_reference_data;

	struct Buffer input_buffer;
	size_t input_offset;

	int fd;
};

struct HOGPDevice {
	struct HOGPInterface interface[INTERFACES_MAX];
	size_t nInterfaces;
	char battery_path[PATH_MAX];
	sd_bus_slot* battery_slot;

	struct GattService devinfo;
	struct GattService battery;

	struct GattCharacteristic appearance;
	struct GattCharacteristic pnp;
	struct GattCharacteristic battery_level;

	struct PnPID pnp_data;
	uint16_t appearance_data;
};

struct BtExtra {
	char* battery;
	unsigned long hci;
	bool hci_chosen;
};

static const sd_bus_vtable gatt_profile[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_PROPERTY("UUIDs", "as", read_string_array, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END
};

static int register_application_cb(sd_bus_message*, void*, sd_bus_error* error) {
	if (sd_bus_error_is_set(error)) {
		log_fmt(ERROR, "Failed to register application: %s\n", error->message);
		sd_bus_error_free(error);
		did_error = true;
		did_hup = true;
	}
	return 0;
}

static int register_advert_cb(sd_bus_message*, void*, sd_bus_error* error) {
	if (sd_bus_error_is_set(error)) {
		log_fmt(ERROR, "Failed to register advertisement: %s\n", error->message);
		sd_bus_error_free(error);
		did_error = true;
		did_hup = true;
	}
	return 0;
}

static const sd_bus_vtable le_advertisement[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_PROPERTY("Type", "s", read_string_indirect, offsetof(struct LEAdvertisement, type), SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("ServiceUUIDs", "as", read_string_array, offsetof(struct LEAdvertisement, uuids), SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("LocalName", "s", read_string_indirect, offsetof(struct LEAdvertisement, local_name), SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("Appearance", "q", read_uint16, offsetof(struct LEAdvertisement, appearance), SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("Duration", "q", read_uint16, offsetof(struct LEAdvertisement, duration), SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("Timeout", "q", read_uint16, offsetof(struct LEAdvertisement, timeout), SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END
};

static int hid_control(const void* data, unsigned size, size_t, unsigned, void* userdata) {
	struct HOGPInterface* iface = userdata;
	(void) data;
	(void) size;
	(void) iface;
	/* TODO: Set up HID Control Point characteristic */
	/* TODO: Do we actually care about these? */
	return 0;
}

static int output_report(const void* data, unsigned size, size_t offset, unsigned mtu, void* userdata) {
	struct HOGPInterface* iface = userdata;
	if (offset >= iface->output_report.data.size || offset + size >= iface->output_report.data.size || size >= iface->output_report.data.size) {
		return -ENOSPC;
	}
	memcpy((uint8_t*) iface->output_report.data.data + offset, data, size);
	if (size < mtu) {
		return write(iface->fd, iface->output_report.data.data, offset + size) > 0;
	}

	return 0;
}

static int feature_report(const void* data, unsigned size, size_t offset, unsigned mtu, void* userdata) {
	struct HOGPInterface* iface = userdata;
	if (!offset) {
		--size;
	} else {
		--offset;
	}
	if (offset >= iface->feature_report.data.size || offset + size > iface->feature_report.data.size || size > iface->feature_report.data.size) {
		return -ENOSPC;
	}
	memcpy((uint8_t*) iface->feature_report.data.data + offset, data, size);
	log_fmt(DEBUG, "Feature report data in: ");
	unsigned i;
	for (i = 0; i < iface->feature_report.data.size; ++i) {
		log_fmt(DEBUG, " %02X", ((uint8_t*) iface->feature_report.data.data)[i]);
	}
	log_fmt(DEBUG, "\n");
	if (size < mtu) {
		int res;
		res = ioctl(iface->fd, HIDIOCSFEATURE(offset + size), iface->feature_report.data.data);
		if (res < 0) {
			perror("SET ioctl out failed");
			return res;
		}
		res = ioctl(iface->fd, HIDIOCGFEATURE(64), iface->feature_report.data.data);
		if (res < 0) {
			perror("GET ioctl in failed");
			return res;
		}
		log_fmt(DEBUG, "Feature report data out:");
		for (i = 0; i < iface->feature_report.data.size; ++i) {
			log_fmt(DEBUG, " %02X", ((uint8_t*) iface->feature_report.data.data)[i]);
		}
		log_fmt(DEBUG, "\n");
	}

	return 0;
}

void hogp_create_interface(struct HOGPInterface* iface) {
	iface->hid_info_data.bcdHID = htobs(0x111);
	iface->hid_info_data.bCountryCode = 0;
	iface->hid_info_data.flags = 0;

	iface->input_report_reference_data.reportId = 0;
	iface->input_report_reference_data.reportType = REPORT_TYPE_INPUT;

	iface->output_report_reference_data.reportId = 0;
	iface->output_report_reference_data.reportType = REPORT_TYPE_OUTPUT;

	iface->feature_report_reference_data.reportId = 0;
	iface->feature_report_reference_data.reportType = REPORT_TYPE_FEATURE;

	gatt_characteristic_create(&iface->hid_info, UUID_HID_INFO, &iface->hid);
	iface->hid_info.flags = GATT_FLAG_READ;
	iface->hid_info.data.data = &iface->hid_info_data;
	iface->hid_info.data.size = sizeof(iface->hid_info_data);

	gatt_characteristic_create(&iface->report_map, UUID_REPORT_MAP, &iface->hid);
	iface->report_map.flags = GATT_FLAG_READ;
	buffer_create(&iface->report_map.data);

	gatt_characteristic_create(&iface->hid_control, UUID_HID_CONTROL, &iface->hid);
	iface->hid_control.flags = GATT_FLAG_WRITE_NO_RESPONSE;
	iface->hid_control.write = hid_control;
	buffer_create(&iface->hid_control.data);

	gatt_characteristic_create(&iface->input_report, UUID_REPORT, &iface->hid);
	iface->input_report.flags = GATT_FLAG_RW | GATT_FLAG_NOTIFY;
	iface->input_report.write = output_report;
	iface->input_report.userdata = iface;
	buffer_create(&iface->input_report.data);

	gatt_characteristic_create(&iface->output_report, UUID_REPORT, &iface->hid);
	iface->output_report.flags = GATT_FLAG_RW | GATT_FLAG_WRITE_NO_RESPONSE;
	iface->output_report.write = output_report;
	iface->output_report.userdata = iface;
	buffer_create(&iface->output_report.data);
	buffer_alloc(&iface->output_report.data, REPORT_SIZE_MAX);

	gatt_characteristic_create(&iface->feature_report, UUID_REPORT, &iface->hid);
	iface->feature_report.flags = GATT_FLAG_RW;
	iface->feature_report.write = feature_report;
	iface->feature_report.userdata = iface;
	buffer_create(&iface->feature_report.data);
	buffer_alloc(&iface->feature_report.data, 64);

	gatt_descriptor_create(&iface->input_report_reference, UUID_REPORT_REFERENCE, &iface->input_report);
	iface->input_report_reference.flags = GATT_FLAG_READ;
	iface->input_report_reference.data.data = &iface->input_report_reference_data;
	iface->input_report_reference.data.size = sizeof(iface->input_report_reference_data);

	gatt_descriptor_create(&iface->output_report_reference, UUID_REPORT_REFERENCE, &iface->output_report);
	iface->output_report_reference.flags = GATT_FLAG_READ;
	iface->output_report_reference.data.data = &iface->output_report_reference_data;
	iface->output_report_reference.data.size = sizeof(iface->output_report_reference_data);

	gatt_descriptor_create(&iface->feature_report_reference, UUID_REPORT_REFERENCE, &iface->feature_report);
	iface->feature_report_reference.flags = GATT_FLAG_READ;
	iface->feature_report_reference.data.data = &iface->feature_report_reference_data;
	iface->feature_report_reference.data.size = sizeof(iface->feature_report_reference_data);

	buffer_create(&iface->input_buffer);
	buffer_alloc(&iface->input_buffer, 256);
	iface->input_offset = 0;

	iface->id = 0;
	iface->fd = -1;
}

void hogp_create(struct HOGPDevice* hog, const char* namespace, size_t nInterfaces) {
	char path[PATH_MAX];
	size_t i;

	log_fmt(DEBUG, "Creating HID-Over-GATT profile device with %zu interfaces\n", nInterfaces);
	hog->pnp_data.source = 2;
	hog->appearance_data = htobs(GAP_GAMEPAD);
	hog->battery_slot = NULL;

	snprintf(path, sizeof(path), "%s/dis", namespace);
	gatt_service_create(&hog->devinfo, UUID_DEV_INFO, path);
	gatt_characteristic_create(&hog->pnp, UUID_PNP_ID, &hog->devinfo);
	hog->pnp.flags = GATT_FLAG_READ;
	hog->pnp.data.data = &hog->pnp_data;
	hog->pnp.data.size = sizeof(hog->pnp_data);

	snprintf(path, sizeof(path), "%s/bas", namespace);
	gatt_service_create(&hog->battery, UUID_BATTERY, path);
	gatt_characteristic_create(&hog->battery_level, UUID_BATTERY_LEVEL, &hog->battery);
	hog->battery_level.flags = GATT_FLAG_READ;
	buffer_create(&hog->battery_level.data);
	buffer_alloc(&hog->battery_level.data, 1);
	*(uint8_t*) hog->battery_level.data.data = 100;

	hog->nInterfaces = nInterfaces;
	memset(hog->interface, 0, sizeof(hog->interface));
	for (i = 0; i < nInterfaces; ++i) {
		snprintf(path, sizeof(path), "%s/iface%04zx", namespace, i);
		gatt_service_create(&hog->interface[i].hid, UUID_HID, path);
		hogp_create_interface(&hog->interface[i]);
	}
}

void hogp_destroy_interface(struct HOGPInterface* iface) {
	gatt_service_destroy(&iface->hid);
	buffer_destroy(&iface->input_report.data);
	buffer_destroy(&iface->output_report.data);
	buffer_destroy(&iface->feature_report.data);
	buffer_destroy(&iface->report_map.data);
	buffer_destroy(&iface->hid_control.data);
	buffer_destroy(&iface->input_buffer);
	close(iface->fd);
}

void hogp_destroy(struct HOGPDevice* hog) {
	size_t i;
	gatt_service_destroy(&hog->devinfo);
	gatt_service_destroy(&hog->battery);

	for (i = 0; i < hog->nInterfaces; ++i) {
		hogp_destroy_interface(&hog->interface[i]);
	}
	sd_bus_slot_unref(hog->battery_slot);
}

static int hogp_update_battery(sd_bus_message* m, void* userdata, sd_bus_error*) {
	struct HOGPDevice* hog = userdata;
	int res;
	const char* iface;
	const char* property;

	res = sd_bus_message_read_basic(m, 's', &iface);
	if (res < 0) {
		return res;
	}

	if (strcmp(iface, "org.freedesktop.UPower.Device") != 0) {
		return 0;
	}

	res = sd_bus_message_enter_container(m, 'a', "{sv}");
	if (res < 0) {
		return res;
	}
	while ((res = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		char type;
		res = sd_bus_message_read_basic(m, 's', &property);
		if (res < 0) {
			return res;
		}
		res = sd_bus_message_peek_type(m, &type, NULL);
		if (res < 0) {
			return res;
		}

		if (strcasecmp(property, "Percentage") != 0) {
			sd_bus_message_skip(m, NULL);
		} else {
			double percentage;
			res = sd_bus_message_read(m, "v", "d", &percentage);
			if (res < 0) {
				return res;
			}
			*(uint8_t*) hog->battery_level.data.data = percentage;
		}

		res = sd_bus_message_exit_container(m);
		if (res < 0) {
			return res;
		}
	}

	res = sd_bus_message_exit_container(m);
	if (res < 0) {
		return res;
	}
	return 0;
}

int hogp_register(struct HOGPDevice* hog, sd_bus* bus) {
	size_t i;
	int res;

	res = gatt_service_register(&hog->devinfo, bus);
	if (res < 0) {
		log_fmt(ERROR, "Failed to publish device info service: %s\n", strerror(-res));
		return res;
	}

	res = gatt_service_register(&hog->battery, bus);
	if (res < 0) {
		log_fmt(ERROR, "Failed to publish battery service: %s\n", strerror(-res));
		return res;
	}

	for (i = 0; i < hog->nInterfaces; ++i) {
		res = gatt_service_register(&hog->interface[i].hid, bus);
		if (res < 0) {
			log_fmt(ERROR, "Failed to publish HID service: %s\n", strerror(-res));
			return res;
		}
	}

	res = sd_bus_match_signal_async(bus, &hog->battery_slot, "org.freedesktop.UPower", hog->battery_path, "org.freedesktop.DBus.Properties", "PropertiesChanged", hogp_update_battery, NULL, hog);
	if (res < 0) {
		log_fmt(WARN, "Failed to subscribe to battery updates: %s\n", strerror(-res));
		/* This is a non-fatal error, so don't return failure */
	}
	return 0;
}

bool hogp_setup(struct HOGPDevice* hog, const char* syspath, const char* bus_id) {
	int fd;
	uint16_t u16;
	char interface[PATH_MAX];
	char syspath_tmp[PATH_MAX];
	char report_descriptor[DESCRIPTOR_SIZE_MAX];
	ssize_t desc_size;
	size_t i;

	fd = vopen("%s/idVendor", O_RDONLY, 0666, syspath);
	if (fd < 0) {
		return false;
	}
	if (!read_u16(fd, &u16)) {
		close(fd);
		return false;
	}
	hog->pnp_data.vid = u16;
	close(fd);

	fd = vopen("%s/idProduct", O_RDONLY, 0666, syspath);
	if (fd < 0) {
		return false;
	}
	if (!read_u16(fd, &u16)) {
		close(fd);
		return false;
	}
	hog->pnp_data.pid = u16;
	close(fd);

	for (i = 0; i < hog->nInterfaces; ++i) {
		int type = interface_type(syspath, bus_id, i);
		if (type != 3) {
			continue;
		}

		hog->interface[i].id = i;
		snprintf(syspath_tmp, sizeof(syspath_tmp), "%s/%s:1.%lu", syspath, bus_id, i);
		if (!find_function(syspath_tmp, interface, sizeof(interface))) {
			puts("Failed to find function");
			return false;
		}
		fd = vopen("%s/report_descriptor", O_RDONLY, 0666, interface);

		desc_size = read(fd, report_descriptor, sizeof(report_descriptor));
		if (desc_size <= 0) {
			perror("Failed to read report descriptor file");
			return false;
		}

		buffer_realloc(&hog->interface[i].report_map.data, desc_size);
		memcpy(hog->interface[i].report_map.data.data, report_descriptor, desc_size);

		hog->interface[i].fd = find_hidraw(syspath_tmp);
	}

	/* TODO: Figure out Report Map characteristic descriptors? */

	return true;
}

bool poll_fds(sd_bus* bus, struct HOGPDevice* dev) {
	struct pollfd fds[INTERFACES_MAX];
	size_t i;
	int res;
	uint8_t buffer[REPORT_SIZE_MAX];
	ssize_t sizein;
	uint64_t last_flush;
	uint64_t timestamp;
	bool do_process = true;
	bool do_flush = false;
	bool flush_this = false;
	bool is_deck = dev->pnp_data.vid == VID_VALVE && dev->pnp_data.pid == PID_STEAM_DECK;
	struct timespec ts;

	for (i = 0; i < dev->nInterfaces; ++i) {
		fds[i].fd = dev->interface[i].fd;
		fds[i].events = POLLIN | POLLPRI;
	}

	clock_gettime(CLOCK_MONOTONIC, &ts);
	last_flush = ts.tv_sec * 1000000000ULL + ts.tv_nsec;

	while (!did_hup) {
		if (do_process) {
			res = sd_bus_process(bus, NULL);
			if (res < 0) {
				log_fmt(ERROR, "Failed to process bus: %s\n", strerror(-res));
				break;
			}

			if (res > 0) {
				continue;
			}

			do_process = false;
		}

		res = sd_bus_wait(bus, 1);
		if (res < 0 && res != -EINTR) {
			log_fmt(ERROR, "Failed to wait on bus: %s\n", strerror(-res));
			return false;
		}
		if (res > 0) {
			do_process = true;
			continue;
		}

		res = poll(fds, dev->nInterfaces, 4);
		if (res == -EAGAIN) {
			continue;
		}
		if (res < 0) {
			if (errno != EINTR) {
				perror("Failed to poll nodes");
			}
			return did_hup;
		}

		clock_gettime(CLOCK_MONOTONIC, &ts);
		timestamp = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
		do_flush = timestamp - last_flush >= FLUSH_INTERVAL;
		if (do_flush) {
			log_fmt(DEBUG, "Timeout triggering flush\n");
			last_flush = timestamp;
		}

		for (i = 0; i < dev->nInterfaces; ++i) {
			flush_this = true;
			sizein = 0;
			if (fds[i].revents & POLLIN) {
				sizein = read(fds[i].fd, buffer, sizeof(buffer));
				if (sizein < 0) {
					if (errno != EINTR) {
						perror("Failed to read packet");
					}
					return did_hup;
				}
				fds[i].revents &= ~POLLIN;
				if (dev->interface[i].input_report.notify_fd < 0) {
					continue;
				}

				if ((size_t) sizein > dev->interface[i].input_buffer.size) {
					buffer_realloc(&dev->interface[i].input_buffer, sizein);
				}
				if (!do_flush && is_deck && i == DECK_RAW_IFACE) {
					flush_this = filter_update(&deck_filter, dev->interface[i].input_buffer.data, buffer, sizein);
					if (flush_this) {
						log_fmt(DEBUG, "Filter triggering flush on interface %zu\n", i);
						if (timestamp - last_flush < FLUSH_THROTTLE) {
							log_fmt(DEBUG, "Flushing too fast, throttling...\n");
							flush_this = false;
						} else {
							last_flush = timestamp;
						}
					}
				}
				if (flush_this) {
					memcpy(dev->interface[i].input_buffer.data, buffer, sizein);
				}
				dev->interface[i].input_offset = sizein;
			} else if (is_deck && i == DECK_RAW_IFACE) {
				flush_this = do_flush;
			}

			sizein = dev->interface[i].input_offset;
			if (!sizein || !(flush_this || do_flush)) {
				continue;
			}
			res = write(dev->interface[i].input_report.notify_fd, dev->interface[i].input_buffer.data, sizein);
			if (res < 0) {
				if (errno == EAGAIN) {
					continue;
				}
				if (errno != EINTR) {
					perror("Failed to write packet");
				}
				return did_hup;
			}
			dev->interface[i].input_offset = 0;
			log_fmt(DEBUG, "Flushed interface %zu\n", i);
		}
	}
	return true;
}

static bool bto_parse(void* userdata, int c) {
	struct BtExtra* extra = userdata;
	char* end;
	switch (c) {
	case 'b':
		extra->battery = strdup(optarg);
		return true;
	case 'i':
		extra->hci = strtoul(optarg, &end, 0);
		if (!end[0]) {
			extra->hci_chosen = true;
			return true;
		}
		break;
	}
	return false;
}

static void bto_free(void* userdata) {
	struct BtExtra* extra = userdata;
	free(extra->battery);
}

int main(int argc, char* argv[]) {
	char syspath[PATH_MAX];
	char bus_id[32];
	char gatt_manager[PATH_MAX];
	char dbus_path[PATH_MAX] = {0};
	bool is_hid[INTERFACES_MAX] = {0};
	const char* name;
	const char* battery = "battery_BAT1";
	struct sigaction sa;
	int max_interfaces;
	int ok = 1;
	int hci;
	sd_bus* bus;
	sd_bus_slot* object_manager_slot = NULL;
	sd_bus_slot* register_service_slot = NULL;
	sd_bus_slot* profile_slot = NULL;
	sd_bus_slot* register_advert_slot = NULL;
	sd_bus_slot* advert_slot = NULL;
	sd_bus_message* reply = NULL;
	sd_bus_error error;
	int res;
	int i;
	struct HOGPDevice hog;
	struct LEAdvertisement advertisement = {
		.type = "peripheral",
		.uuids = (const char*[]) {UUID_DEV_INFO, UUID_HID, UUID_BATTERY, NULL},
		.local_name = "USB Gamepad",
		.appearance = htobs(GAP_GAMEPAD),
	};
	struct BtExtra bt_extra = {0};
	struct OptionsExtra extra = {
		":b:i:",
		(struct option[]) {
			{"--battery", required_argument, 0, 'b'},
			{"--hci", required_argument, 0, 'i'},
			{0}
		},
		bto_parse,
		bto_free,
		(const char*[]) {
			" -b, --battery BAT  Select which battery to relay over the Battery Service",
			" -i, --hci INDEX    Select the index of the HCI to use",
			NULL
		},
		&bt_extra
	};
	struct Options opts = {0};

	const char* profile[] = {
		UUID_HID,
		NULL
	};

	opts.extra = &extra;
	if (!getopt_parse(argc, argv, &opts)) {
		usage(argv[0], false, &extra);
		goto shutdown;
	}
	if (opts.usage) {
		usage(argv[0], true, &extra);
		getopt_free(&opts);
		return 0;
	}

	/* Resolve paths to sysfs nodes */
	if (!find_sysfs_path(opts.dev, syspath, bus_id)) {
		goto shutdown;
	}

	max_interfaces = interface_count(syspath);
	if (max_interfaces < 0) {
		goto shutdown;
	}

	if (max_interfaces > INTERFACES_MAX) {
		max_interfaces = INTERFACES_MAX;
	}

	/* Get the number of HID interfaces */
	for (i = 0; i < max_interfaces; ++i) {
		int type = interface_type(syspath, bus_id, i);
		is_hid[i] = type == 3;
	}
	max_interfaces = 0;
	for (i = 0; i < INTERFACES_MAX; ++i) {
		max_interfaces += is_hid[i];
	}

	/* Set up D-Bus/BlueZ */
	if (bt_extra.hci_chosen) {
		hci = bt_extra.hci;
	} else {
		hci = hci_get_route(NULL);
		if (hci < 0) {
			perror("Failed to get default HCI");
			goto shutdown;
		}
	}

	snprintf(gatt_manager, sizeof(gatt_manager), "/org/bluez/hci%i", hci);

	res = sd_bus_open_system(&bus);
	if (res < 0) {
		log_fmt(ERROR, "Failed to connect to system D-Bus: %s\n", strerror(-res));
		goto shutdown;
	}

	if (opts.name) {
		snprintf(dbus_path, sizeof(dbus_path) - 1, "/%s", opts.name);
	} else {
		strncpy(dbus_path, "/com/valvesoftware/usbhid_passthru", sizeof(dbus_path) - 1);
	}

	res = sd_bus_add_object_vtable(bus, &profile_slot, dbus_path,
		 "org.bluez.GattProfile1", gatt_profile, profile);
	if (res < 0) {
		log_fmt(ERROR, "Failed to publish profile: %s\n", strerror(-res));
		goto shutdown;
	}

	/* Create HoG device */
	hogp_create(&hog, dbus_path, max_interfaces);

	if (!hogp_setup(&hog, syspath, bus_id)) {
		goto shutdown;
	}

	if (bt_extra.battery) {
		battery = bt_extra.battery;
	}
	snprintf(hog.battery_path, sizeof(hog.battery_path), "/org/freedesktop/UPower/devices/%s", battery);

	res = hogp_register(&hog, bus);
	if (res < 0) {
		log_fmt(ERROR, "Failed to publish HOGP: %s\n", strerror(-res));
		goto shutdown;
	}

	/* Publish all of it */
	res = sd_bus_add_object_vtable(bus, &advert_slot, dbus_path,
		 "org.bluez.LEAdvertisement1", le_advertisement, &advertisement);
	if (res < 0) {
		log_fmt(ERROR, "Failed to publish advertisement: %s\n", strerror(-res));
		goto shutdown;
	}

	res = sd_bus_add_object_manager(bus, &object_manager_slot, dbus_path);
	if (res < 0) {
		log_fmt(ERROR, "Failed to add object manager: %s\n", strerror(-res));
		goto shutdown;
	}

	res = sd_bus_call_method_async(bus, &register_advert_slot, "org.bluez", gatt_manager, "org.bluez.LEAdvertisingManager1",
		"RegisterAdvertisement", register_advert_cb, &advertisement, "oa{sv}", dbus_path, 0, NULL);
	if (res < 0) {
		log_fmt(ERROR, "Failed to register advertisement: %s\n", error.message);
		goto shutdown;
	}

	res = sd_bus_call_method_async(bus, &register_service_slot, "org.bluez", gatt_manager, "org.bluez.GattManager1",
		"RegisterApplication", register_application_cb, NULL, "oa{sv}", dbus_path, 0, NULL);
	if (res < 0) {
		log_fmt(ERROR, "Failed to register application: %s\n", strerror(-res));
		goto shutdown;
	}

	/* We want to exit cleanly in event of SIGINT or SIGHUP */
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = hup;
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	if (sd_bus_get_unique_name(bus, &name) == 0) {
		log_fmt(INFO, "D-Bus name: %s\n", name);
	}

	ok = !poll_fds(bus, &hog);

	if (did_error) {
		ok = 1;
	}

shutdown:
	getopt_free(&opts);
	sd_bus_call_method(bus, "org.bluez", gatt_manager, "org.bluez.GattManager1", "UnregisterApplication",
		&error, &reply, "o", dbus_path);
	sd_bus_message_unref(reply);
	sd_bus_call_method(bus, "org.bluez", gatt_manager, "org.bluez.LEAdvertisingManager1", "UnregisterAdvertisement",
		&error, &reply, "o", dbus_path);
	sd_bus_message_unref(reply);
	sd_bus_slot_unref(object_manager_slot);
	sd_bus_slot_unref(register_service_slot);
	hogp_destroy(&hog);
	sd_bus_slot_unref(profile_slot);
	sd_bus_slot_unref(register_advert_slot);
	sd_bus_slot_unref(advert_slot);
	sd_bus_unref(bus);

	return ok;
}
