/* SPDX-License-Identifier: BSD-3-Clause */
#include "dev.h"
#include "usb.h"
#include "util.h"

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>

#define HIDP_CONTROL_PSM 0x11
#define HIDP_INTERRUPT_PSM 0x13
#define UUID_HID "00001812-0000-1000-8000-00805f9b34fb"
#define UUID_REPORT "00002a4d-0000-1000-8000-00805f9b34fb"
#define UUID_PNP_ID "00002a50-0000-1000-8000-00805f9b34fb"
#define GAP_GAMEPAD 0x03C4

bool did_hup = false;
bool did_error = false;

void hup() {
	did_hup = true;
}

struct GattService {
	bool primary;
	char uuid[37];
};

struct GattCharacteristic {
	char uuid[37];
	const char* service;
	const char** flags;
	uint16_t mtu;
	void* data;
	size_t size;
};

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

static int read_bool(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userdata, sd_bus_error*) {
	bool b = *(bool*) userdata;
	return sd_bus_message_append_basic(reply, 'b', &b);
}

static int read_uint16(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userdata, sd_bus_error*) {
	uint16_t q = *(uint16_t*) userdata;
	return sd_bus_message_append_basic(reply, 'q', &q);
}

static int read_object_indirect(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userdata, sd_bus_error*) {
	const char* direct = *(const char**) userdata;
	return sd_bus_message_append(reply, "o", direct);
}

static int read_string(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userdata, sd_bus_error*) {
	return sd_bus_message_append(reply, "s", userdata);
}

static int read_string_indirect(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userdata, sd_bus_error*) {
	const char* direct = *(const char**) userdata;
	return sd_bus_message_append(reply, "s", direct);
}

static int read_string_array(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userdata, sd_bus_error*) {
	const char** strings = *(const char***) userdata;
	size_t i;
	int res;

	res = sd_bus_message_open_container(reply, 'a', "s");
	if (res < 0) {
		return res;
	}

	for (i = 0; strings && strings[i]; i++) {
		res = sd_bus_message_append(reply, "s", strings[i]);
		if (res < 0) {
			return res;
		}
	}

	return sd_bus_message_close_container(reply);
}

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

static int read_characteristic(sd_bus_message* m, void *userdata, sd_bus_error* error) {
	struct GattCharacteristic* characteristic = userdata;
	const char* opt;
	int res;
	size_t offset = 0;
	size_t length = characteristic->size;
	sd_bus_message* reply;

	res = sd_bus_message_enter_container(m, 'a', "{sv}");
	if (res < 0) {
		return res;
	}

	while ((res = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		res = sd_bus_message_read(m, "s", &opt);
		if (res < 0) {
			return res;
		}
		puts(opt);
		if (strcasecmp(opt, "offset") == 0) {
			char type;
			uint16_t read_offset;
			res = sd_bus_message_peek_type(m, &type, NULL);
			if (res < 0) {
				return res;
			}
			if (type != 'q') {
				return -EINVAL;
			}

			res = sd_bus_message_read(m, "q", &read_offset);
			if (res < 0) {
				return res;
			}

			if (read_offset > length) {
				return sd_bus_error_setf(error, "org.bluez.Error.InvalidOffset", "Requested offset %u exceeds characteristic size %zu", read_offset, length);
			}
			offset = read_offset;
			length -= read_offset;
		}

		res = sd_bus_message_exit_container(m);
		if (res < 0) {
			return res;
		}
	}
	if (res < 0) {
		return res;
	}

	res = sd_bus_message_exit_container(m);
	if (res < 0) {
		return res;
	}

	res = sd_bus_message_new_method_return(m, &reply);
	if (res < 0) {
		return res;
	}

	res = sd_bus_message_append_array(reply, 'y', &((uint8_t*) characteristic->data)[offset], length);
	if (res < 0) {
		return res;
	}

	return sd_bus_message_send(reply);
}

static const sd_bus_vtable gatt_service[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_PROPERTY("Primary", "b", read_bool, offsetof(struct GattService, primary), SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("UUID", "s", read_string, offsetof(struct GattService, uuid), SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END
};

static const sd_bus_vtable gatt_profile[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_PROPERTY("UUIDs", "as", read_string_array, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END
};

static const sd_bus_vtable gatt_characteristic[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_PROPERTY("UUID", "s", read_string, offsetof(struct GattCharacteristic, uuid), SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("Service", "o", read_object_indirect, offsetof(struct GattCharacteristic, service), SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("Flags", "as", read_string_array, offsetof(struct GattCharacteristic, flags), SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("MTU", "q", read_uint16, offsetof(struct GattCharacteristic, mtu), SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_METHOD_WITH_ARGS("ReadValue", "a{sv}", "ay", read_characteristic, 0),
	SD_BUS_VTABLE_END
};

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

int main(int argc, char* argv[]) {
	char syspath[PATH_MAX];
	char bus_id[32];
	char gatt_manager[PATH_MAX];
	struct sigaction sa;
	int ok = 1;
	int hci;
	sd_bus* bus;
	sd_bus_slot* object_manager_slot = NULL;
	sd_bus_slot* register_service_slot = NULL;
	sd_bus_slot* service_slot = NULL;
	sd_bus_slot* profile_slot = NULL;
	sd_bus_slot* char_pnp_slot = NULL;
	sd_bus_slot* register_advert_slot = NULL;
	sd_bus_slot* advert_slot = NULL;
	sd_bus_message* reply = NULL;
	sd_bus_error error;
	int res;
	struct GattService hid = {
		.primary = true,
		.uuid = UUID_HID
	};
	struct PnPID pnp = {
		.source = 2
	};
	struct GattCharacteristic char_pnp = {
		.uuid = UUID_PNP_ID,
		.flags = (const char*[]) {"read", NULL},
		.data = &pnp,
		.size = sizeof(pnp)
	};
	struct LEAdvertisement advertisement = {
		.type = "peripheral",
		.uuids = (const char*[]) {UUID_HID, NULL},
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

	res = sd_bus_add_object_vtable(bus, &service_slot, "/com/valvesoftware/Deck/service0",
		 "org.bluez.GattService1", gatt_service, &hid);
	if (res < 0) {
		printf("Failed to publish service: %s\n", strerror(-res));
		goto shutdown;
	}

	char_pnp.service = "/com/valvesoftware/Deck/service0";
	res = sd_bus_add_object_vtable(bus, &char_pnp_slot, "/com/valvesoftware/Deck/service0/char0000",
		 "org.bluez.GattCharacteristic1", gatt_characteristic, &char_pnp);
	if (res < 0) {
		printf("Failed to publish service: %s\n", strerror(-res));
		goto shutdown;
	}

	res = sd_bus_add_object_vtable(bus, &advert_slot, "/com/valvesoftware/Deck/advert",
		 "org.bluez.LEAdvertisement1", le_advertisement, &advertisement);
	if (res < 0) {
		printf("Failed to publish service: %s\n", strerror(-res));
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

	/* We want to exit cleanly in event of SIGINT or SIGHUP */
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = hup;
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	ok = 0;
	while (!did_hup) {
		sd_bus_message* m;
		res = sd_bus_process(bus, &m);
		if (res < 0) {
			printf("Failed to process bus: %s\n", strerror(-res));
			break;
		}
		if (m) {
			printf("path: %s\n", sd_bus_message_get_path(m));
			printf("iface: %s\n", sd_bus_message_get_interface(m));
			printf("member: %s\n", sd_bus_message_get_member(m));
			printf("sender: %s\n", sd_bus_message_get_sender(m));
			puts("");
			sd_bus_message_unref(m);
		}
		if (res > 0) {
			continue;
		}

		/* Wait for the next request to process */
		res = sd_bus_wait(bus, UINT64_MAX);
		if (res == -EINTR) {
			continue;
		}
		if (res < 0) {
			printf("Failed to wait on bus: %s\n", strerror(-res));
			break;
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
	sd_bus_slot_unref(service_slot);
	sd_bus_slot_unref(profile_slot);
	sd_bus_slot_unref(char_pnp_slot);
	sd_bus_slot_unref(register_advert_slot);
	sd_bus_slot_unref(advert_slot);
	sd_bus_unref(bus);

	return ok;
}
