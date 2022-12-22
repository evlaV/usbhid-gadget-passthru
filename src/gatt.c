/* SPDX-License-Identifier: BSD-3-Clause */
#include "dbus.h"
#include "gatt.h"

#include <dirent.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>

static int read_characteristic(sd_bus_message* m, void *userdata, sd_bus_error* error);

static const sd_bus_vtable gatt_service[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_PROPERTY("Primary", "b", read_bool, offsetof(struct GattService, primary), SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("UUID", "s", read_string, offsetof(struct GattService, uuid), SD_BUS_VTABLE_PROPERTY_CONST),
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

void gatt_service_create(struct GattService* service, const char* uuid, const char* path) {
	memset(service, 0, sizeof(*service));
	strncpy(service->uuid, uuid, sizeof(service->uuid) - 1);
	service->primary = true;
	service->path = path;
}

int gatt_service_register(struct GattService* service, sd_bus* bus) {
	size_t i;
	int res = sd_bus_add_object_vtable(bus, &service->slot, service->path,
		"org.bluez.GattService1", gatt_service, service);
	if (res < 0) {
		return res;
	}

	for (i = 0; i < service->nCharacteristics; ++i) {
		char path[PATH_MAX] = {0};
		snprintf(path, sizeof(path) - 1, "%s/char%04zx", service->path, i);
		res = sd_bus_add_object_vtable(bus, &service->characteristics[i]->slot, path,
			 "org.bluez.GattCharacteristic1", gatt_characteristic, service->characteristics[i]);
		if (res < 0) {
			break;
		}
	}
	return res;
};

void gatt_characteristic_create(struct GattCharacteristic* characteristic, const char* uuid, struct GattService* parent) {
	memset(characteristic, 0, sizeof(*characteristic));
	strncpy(characteristic->uuid, uuid, sizeof(characteristic->uuid) - 1);
	characteristic->service = parent->path;

	if (parent->nCharacteristics == MAX_CHAR) {
		abort();
	}

	parent->characteristics[parent->nCharacteristics] = characteristic;
	++parent->nCharacteristics;
}

