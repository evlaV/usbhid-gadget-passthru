/* SPDX-License-Identifier: BSD-3-Clause */
#include "dbus.h"
#include "dev.h"
#include "gatt.h"
#include "usb.h"
#include "util.h"

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/l2cap.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fcntl.h>
#include <unistd.h>

#define HIDP_CONTROL_PSM 0x11
#define HIDP_INTERRUPT_PSM 0x13

#define UUID_DEV_INFO "0000180a-0000-1000-8000-00805f9b34fb"
#define UUID_BATTERY "0000180f-0000-1000-8000-00805f9b34fb"
#define UUID_HID "00001812-0000-1000-8000-00805f9b34fb"

#define UUID_REPORT_REFERENCE "00002908-0000-1000-8000-00805f9b34fb"
#define UUID_BATTERY_LEVEL "00002a19-0000-1000-8000-00805f9b34fb"

#define UUID_HID_INFO "00002a4a-0000-1000-8000-00805f9b34fb"
#define UUID_REPORT_MAP "00002a4b-0000-1000-8000-00805f9b34fb"
#define UUID_HID_CONTROL "00002a4c-0000-1000-8000-00805f9b34fb"
#define UUID_REPORT "00002a4d-0000-1000-8000-00805f9b34fb"

#define UUID_PNP_ID "00002a50-0000-1000-8000-00805f9b34fb"

#define GAP_GAMEPAD 0x03C4

#define REPORT_TYPE_INPUT 1
#define REPORT_TYPE_OUTPUT 2
#define REPORT_TYPE_FEATURE 3

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

struct HOGPDevice {
	struct GattService devinfo;
	struct GattService hid;
	struct GattService battery;

	struct GattCharacteristic pnp;

	struct GattCharacteristic hid_info;
	struct GattCharacteristic report_map;
	struct GattCharacteristic hid_control;
	struct GattCharacteristic input_report;
	struct GattCharacteristic output_report;
	struct GattCharacteristic feature_report;

	struct GattCharacteristic battery_level;

	struct GattDescriptor input_report_reference;
	struct GattDescriptor input_report_client_config;
	struct GattDescriptor output_report_reference;
	struct GattDescriptor feature_report_reference;

	struct PnPID pnp_data;
	struct HIDInfo hid_info_data;

	struct ReportReference input_report_reference_data;
	struct ReportReference output_report_reference_data;
	struct ReportReference feature_report_reference_data;
};

static const sd_bus_vtable gatt_profile[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_PROPERTY("UUIDs", "as", read_string_array, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END
};

static int register_application_cb(sd_bus_message*, void*, sd_bus_error* error) {
	if (sd_bus_error_is_set(error)) {
		printf("Failed to register application: %s\n", error->message);
		sd_bus_error_free(error);
		did_error = true;
		did_hup = true;
	}
	return 0;
}

static int register_advert_cb(sd_bus_message*, void*, sd_bus_error* error) {
	if (sd_bus_error_is_set(error)) {
		printf("Failed to register advertisement: %s\n", error->message);
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

static int create_server(uint16_t psm) {
	struct sockaddr_l2 addr = {
		.l2_family = AF_BLUETOOTH,
		.l2_psm = htobs(psm),
		.l2_bdaddr = *BDADDR_ANY,
	};
	int res;
	int sock = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
	if (sock < 0) {
		return sock;
	}
	res = bind(sock, (const struct sockaddr*) &addr, sizeof(addr));
	if (res < 0) {
		close(sock);
		return res;
	}
	res = listen(sock, 1);
	if (res < 0) {
		close(sock);
		return res;
	}
	return sock;
}

void hogp_create(struct HOGPDevice* hog) {
	hog->pnp_data.source = 2;
	hog->hid_info_data.bcdHID = htobs(0x111);
	hog->hid_info_data.bCountryCode = 0;
	hog->hid_info_data.flags = 0;

	hog->input_report_reference_data.reportId = 0;
	hog->input_report_reference_data.reportType = REPORT_TYPE_INPUT;

	hog->output_report_reference_data.reportId = 0;
	hog->output_report_reference_data.reportType = REPORT_TYPE_OUTPUT;

	hog->feature_report_reference_data.reportId = 0;
	hog->feature_report_reference_data.reportType = REPORT_TYPE_FEATURE;

	gatt_service_create(&hog->devinfo, UUID_DEV_INFO, "/com/valvesoftware/Deck/service0");
	gatt_characteristic_create(&hog->pnp, UUID_PNP_ID, &hog->devinfo);
	hog->pnp.flags = GATT_FLAG_READ;
	hog->pnp.data.data = &hog->pnp_data;
	hog->pnp.data.size = sizeof(hog->pnp_data);

	gatt_service_create(&hog->hid, UUID_HID, "/com/valvesoftware/Deck/service1");
	gatt_characteristic_create(&hog->hid_info, UUID_HID_INFO, &hog->hid);
	hog->hid_info.flags = GATT_FLAG_READ;
	hog->hid_info.data.data = &hog->hid_info_data;
	hog->hid_info.data.size = sizeof(hog->hid_info_data);

	gatt_characteristic_create(&hog->report_map, UUID_REPORT_MAP, &hog->hid);
	hog->report_map.flags = GATT_FLAG_READ;
	buffer_create(&hog->report_map.data);

	gatt_characteristic_create(&hog->hid_control, UUID_HID_CONTROL, &hog->hid);
	hog->hid_control.flags = GATT_FLAG_WRITE_NO_RESPONSE;
	buffer_create(&hog->hid_control.data);

	gatt_characteristic_create(&hog->input_report, UUID_REPORT, &hog->hid);
	hog->input_report.flags = GATT_FLAG_RW | GATT_FLAG_NOTIFY;
	buffer_create(&hog->input_report.data);

	gatt_characteristic_create(&hog->output_report, UUID_REPORT, &hog->hid);
	hog->output_report.flags = GATT_FLAG_RW | GATT_FLAG_WRITE_NO_RESPONSE;
	buffer_create(&hog->output_report.data);

	gatt_characteristic_create(&hog->feature_report, UUID_REPORT, &hog->hid);
	hog->feature_report.flags = GATT_FLAG_RW;
	buffer_create(&hog->feature_report.data);

	gatt_descriptor_create(&hog->input_report_reference, UUID_REPORT_REFERENCE, &hog->input_report);
	hog->input_report_reference.flags = GATT_FLAG_READ;
	hog->input_report_reference.data.data = &hog->input_report_reference_data;
	hog->input_report_reference.data.size = sizeof(hog->input_report_reference_data);

	gatt_descriptor_create(&hog->output_report_reference, UUID_REPORT_REFERENCE, &hog->output_report);
	hog->output_report_reference.flags = GATT_FLAG_READ;
	hog->output_report_reference.data.data = &hog->output_report_reference_data;
	hog->output_report_reference.data.size = sizeof(hog->output_report_reference_data);

	gatt_descriptor_create(&hog->feature_report_reference, UUID_REPORT_REFERENCE, &hog->feature_report);
	hog->feature_report_reference.flags = GATT_FLAG_READ;
	hog->feature_report_reference.data.data = &hog->feature_report_reference_data;
	hog->feature_report_reference.data.size = sizeof(hog->feature_report_reference_data);

	gatt_service_create(&hog->battery, UUID_BATTERY, "/com/valvesoftware/Deck/service2");
	gatt_characteristic_create(&hog->battery_level, UUID_PNP_ID, &hog->battery);
	hog->battery_level.flags = GATT_FLAG_READ;
	hog->battery_level.data.data = "100%";
	hog->battery_level.data.size = 4;
}

void hogp_destroy(struct HOGPDevice* hog) {
	gatt_service_destroy(&hog->devinfo);
	gatt_service_destroy(&hog->hid);
	gatt_service_destroy(&hog->battery);
	buffer_destroy(&hog->input_report.data);
	buffer_destroy(&hog->output_report.data);
	buffer_destroy(&hog->feature_report.data);
}

static int hogp_register(struct HOGPDevice* hog, sd_bus* bus) {
	int res;

	res = gatt_service_register(&hog->devinfo, bus);
	if (res < 0) {
		printf("Failed to publish device info service: %s\n", strerror(-res));
		return res;
	}

	res = gatt_service_register(&hog->hid, bus);
	if (res < 0) {
		printf("Failed to publish HID service: %s\n", strerror(-res));
		return res;
	}

	res = gatt_service_register(&hog->battery, bus);
	if (res < 0) {
		printf("Failed to publish battery service: %s\n", strerror(-res));
		return res;
	}
	return 0;
}

int main(int argc, char* argv[]) {
	char syspath[PATH_MAX];
	char bus_id[32];
	char gatt_manager[PATH_MAX];
	uint16_t u16;
	const char* name;
	struct sigaction sa;
	int ok = 1;
	int intr = -1;
	int ctrl = -1;
	int fd = -1;
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
	struct HOGPDevice hog;
	struct LEAdvertisement advertisement = {
		.type = "peripheral",
		.uuids = (const char*[]) {UUID_DEV_INFO, UUID_HID, UUID_BATTERY, NULL},
		.local_name = "USB Gamepad",
		.appearance = GAP_GAMEPAD,
	};

	const char* profile[] = {
		UUID_HID,
		NULL
	};

	if (argc != 2) {
		puts("Missing argument");
		return 0;
	}

	/* Resolve paths to sysfs nodes */
	if (!find_sysfs_path(argv[1], syspath, bus_id)) {
		return 1;
	}

	/* Set up D-Bus/BlueZ */
	hci = hci_get_route(NULL); /* TODO: Allow passing HCI address? */
	if (hci < 0) {
		perror("Failed to get default HCI");
		return 1;
	}

	snprintf(gatt_manager, sizeof(gatt_manager), "/org/bluez/hci%i", hci);

	res = sd_bus_open_system(&bus);
	if (res < 0) {
		printf("Failed to connect to system D-Bus: %s\n", strerror(-res));
		return 1;
	}

	res = sd_bus_add_object_vtable(bus, &profile_slot, "/com/valvesoftware/Deck",
		 "org.bluez.GattProfile1", gatt_profile, profile);
	if (res < 0) {
		printf("Failed to publish profile: %s\n", strerror(-res));
		goto shutdown;
	}

	/* Create HoG device */
	hogp_create(&hog);

	fd = vopen("%s/idVendor", O_RDONLY, 0666, syspath);
	if (fd < 0) {
		goto shutdown;
	}
	if (!read_u16(fd, &u16)) {
		close(fd);
		goto shutdown;
	}
	hog.pnp_data.vid = u16;
	close(fd);

	fd = vopen("%s/idProduct", O_RDONLY, 0666, syspath);
	if (fd < 0) {
		goto shutdown;
	}
	if (!read_u16(fd, &u16)) {
		close(fd);
		goto shutdown;
	}
	hog.pnp_data.pid = u16;
	close(fd);
	/* TODO: Connect USB device to HoG characteristics */

	res = hogp_register(&hog, bus);
	if (res < 0) {
		printf("Failed to publish HOGP: %s\n", strerror(-res));
		goto shutdown;
	}

	/* Publish all of it */
	res = sd_bus_add_object_vtable(bus, &advert_slot, "/com/valvesoftware/Deck/advert",
		 "org.bluez.LEAdvertisement1", le_advertisement, &advertisement);
	if (res < 0) {
		printf("Failed to publish advertisement: %s\n", strerror(-res));
		goto shutdown;
	}

	res = sd_bus_add_object_manager(bus, &object_manager_slot, "/com/valvesoftware/Deck");
	if (res < 0) {
		printf("Failed to add object manager: %s\n", strerror(-res));
		goto shutdown;
	}

	res = sd_bus_call_method_async(bus, &register_advert_slot, "org.bluez", gatt_manager, "org.bluez.LEAdvertisingManager1",
		"RegisterAdvertisement", register_advert_cb, &advertisement, "oa{sv}", "/com/valvesoftware/Deck", 0, NULL);
	if (res < 0) {
		printf("Failed to register advertisement: %s\n", error.message);
		goto shutdown;
	}

	res = sd_bus_call_method_async(bus, &register_service_slot, "org.bluez", gatt_manager, "org.bluez.GattManager1",
		"RegisterApplication", register_application_cb, NULL, "oa{sv}", "/com/valvesoftware/Deck", 0, NULL);
	if (res < 0) {
		printf("Failed to register application: %s\n", strerror(-res));
		goto shutdown;
	}

	ctrl = create_server(HIDP_CONTROL_PSM);
	if (ctrl < 0) {
		perror("Failed to create control socket");
		goto shutdown;
	}

	intr = create_server(HIDP_INTERRUPT_PSM);
	if (intr < 0) {
		perror("Failed to create interrupt socket");
		goto shutdown;
	}

	/* We want to exit cleanly in event of SIGINT or SIGHUP */
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = hup;
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	if (sd_bus_get_unique_name(bus, &name) == 0) {
		printf("name: %s\n", name);
	}

	ok = 0;
	while (!did_hup) {
		sd_bus_message* m;
		res = sd_bus_process(bus, &m);
		if (res < 0) {
			printf("Failed to process bus: %s\n", strerror(-res));
			break;
		}
		if (m) {
			puts("");
			printf("path: %s\n", sd_bus_message_get_path(m));
			printf("iface: %s\n", sd_bus_message_get_interface(m));
			printf("member: %s\n", sd_bus_message_get_member(m));
			printf("sender: %s\n", sd_bus_message_get_sender(m));
			sd_bus_message_unref(m);
		}
		if (res > 0) {
			continue;
		}

		while (!did_hup) {
			nfds_t nfds = 2;
			struct pollfd fds[2] = {
				{.fd = intr, .events = POLLIN},
				{.fd = ctrl, .events = POLLIN},
			};
			res = poll(fds, nfds, 2);
			if (res > 0) {
				puts("Event!");
			}
			res = sd_bus_wait(bus, 2);
			if (res < 0 && res != -EINTR) {
				printf("Failed to wait on bus: %s\n", strerror(-res));
				did_hup = true;
				did_error = true;
				break;
			}
			if (res > 0) {
				break;
			}
		}
	}

	if (did_error) {
		ok = 1;
	}

shutdown:
	sd_bus_call_method(bus, "org.bluez", gatt_manager, "org.bluez.GattManager1", "UnregisterApplication",
		&error, &reply, "o", "/com/valvesoftware/Deck");
	sd_bus_call_method(bus, "org.bluez", gatt_manager, "org.bluez.LEAdvertisingManager1", "UnregisterAdvertisement",
		&error, &reply, "o", "/com/valvesoftware/Deck");
	sd_bus_slot_unref(object_manager_slot);
	sd_bus_slot_unref(register_service_slot);
	hogp_destroy(&hog);
	sd_bus_slot_unref(profile_slot);
	sd_bus_slot_unref(register_advert_slot);
	sd_bus_slot_unref(advert_slot);
	sd_bus_unref(bus);
	close(ctrl);
	close(intr);

	return ok;
}
