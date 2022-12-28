/* SPDX-License-Identifier: BSD-3-Clause */
#include "dbus.h"
#include "gatt.h"

#include <dirent.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>

static int read_characteristic(sd_bus_message* m, void *userdata, sd_bus_error* error);
static int read_descriptor(sd_bus_message* m, void *userdata, sd_bus_error* error);

static int read_flags(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userdata, sd_bus_error*);
static int read_service_path(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userdata, sd_bus_error*);
static int read_characteristic_path(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userdata, sd_bus_error*);

static const sd_bus_vtable gatt_service[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_PROPERTY("Primary", "b", read_bool, offsetof(struct GattService, primary), SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("UUID", "s", read_string, offsetof(struct GattService, uuid), SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END
};

static const sd_bus_vtable gatt_characteristic[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_PROPERTY("UUID", "s", read_string, offsetof(struct GattCharacteristic, uuid), SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("Service", "o", read_service_path, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("Flags", "as", read_flags, offsetof(struct GattCharacteristic, flags), SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("MTU", "q", read_uint16, offsetof(struct GattCharacteristic, mtu), SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_METHOD_WITH_ARGS("ReadValue", "a{sv}", "ay", read_characteristic, 0),
	SD_BUS_VTABLE_END
};

static const sd_bus_vtable gatt_descriptor[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_PROPERTY("UUID", "s", read_string, offsetof(struct GattDescriptor, uuid), SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("Characteristic", "o", read_characteristic_path, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("Flags", "as", read_flags, offsetof(struct GattDescriptor, flags), SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_METHOD_WITH_ARGS("ReadValue", "a{sv}", "ay", read_descriptor, 0),
	SD_BUS_VTABLE_END
};

static int parse_flags(sd_bus_message* m, size_t* offset, size_t* length, const char* objtype, sd_bus_error* error) {
	const char* opt;
	int res;
	uint16_t mtu = *length;

	res = sd_bus_message_enter_container(m, 'a', "{sv}");
	if (res < 0) {
		return res;
	}

	while ((res = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		char type;
		res = sd_bus_message_read(m, "s", &opt);
		if (res < 0) {
			return res;
		}
		res = sd_bus_message_peek_type(m, &type, NULL);
		if (res < 0) {
			return res;
		}
		if (strcasecmp(opt, "offset") == 0) {
			uint16_t read_offset;

			if (type == 'q') {
				res = sd_bus_message_read(m, "q", &read_offset);
			} else if (type == 'v') {
				res = sd_bus_message_read(m, "v", "q", &read_offset);
			} else {
				return -EINVAL;
			}
			if (res < 0) {
				return res;
			}

			if (read_offset > *length) {
				return sd_bus_error_setf(error, "org.bluez.Error.InvalidOffset", "Requested offset %u exceeds %s size %zu", read_offset, objtype, *length);
			}
			*offset = read_offset;
			*length -= read_offset;
		} else if (strcasecmp(opt, "mtu") == 0) {
			if (type == 'q') {
				res = sd_bus_message_read(m, "q", &mtu);
			} else if (type == 'v') {
				res = sd_bus_message_read(m, "v", "q", &mtu);
			} else {
				return -EINVAL;
			}
			if (res < 0) {
				return res;
			}
		} else {
			printf("Unhandled flag: %s : %c\n", opt, type);
			sd_bus_message_skip(m, NULL);
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

	if (mtu < *length) {
		*length = mtu;
	}
	return 0;
}

static int read_characteristic(sd_bus_message* m, void *userdata, sd_bus_error* error) {
	struct GattCharacteristic* characteristic = userdata;
	int res;
	size_t offset = 0;
	size_t length = characteristic->data.size;
	sd_bus_message* reply;

	if (!(characteristic->flags & GATT_FLAG_READ)) {
		return sd_bus_error_set(error, "org.bluez.Error.NotSupported", "Reading not supported");
	}

	res = parse_flags(m, &offset, &length, "characteristic", error);
	if (res < 0 || sd_bus_error_is_set(error)) {
		return res;
	}

	res = sd_bus_message_new_method_return(m, &reply);
	if (res < 0) {
		return res;
	}

	res = sd_bus_message_append_array(reply, 'y', &((uint8_t*) characteristic->data.data)[offset], length);
	if (res < 0) {
		return res;
	}

	return sd_bus_message_send(reply);
}

static int read_descriptor(sd_bus_message* m, void *userdata, sd_bus_error* error) {
	struct GattDescriptor* descriptor = userdata;
	int res;
	size_t offset = 0;
	size_t length = descriptor->data.size;
	sd_bus_message* reply;

	if (!(descriptor->flags & GATT_FLAG_READ)) {
		return sd_bus_error_set(error, "org.bluez.Error.NotSupported", "Reading not supported");
	}

	res = parse_flags(m, &offset, &length, "descriptor", error);
	if (res < 0 || sd_bus_error_is_set(error)) {
		return res;
	}

	res = sd_bus_message_new_method_return(m, &reply);
	if (res < 0) {
		return res;
	}

	res = sd_bus_message_append_array(reply, 'y', &((uint8_t*) descriptor->data.data)[offset], length);
	if (res < 0) {
		return res;
	}

	return sd_bus_message_send(reply);
}

static int read_flags(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userdata, sd_bus_error*) {
	uint32_t flags = *(uint32_t*) userdata;
	int res;

	res = sd_bus_message_open_container(reply, 'a', "s");
	if (res < 0) {
		return res;
	}

	if (flags & GATT_FLAG_READ) {
		res = sd_bus_message_append(reply, "s", "read");
		if (res < 0) {
			return res;
		}
	}

	if (flags & GATT_FLAG_WRITE) {
		res = sd_bus_message_append(reply, "s", "write");
		if (res < 0) {
			return res;
		}
	}

	if (flags & GATT_FLAG_WRITE_NO_RESPONSE) {
		res = sd_bus_message_append(reply, "s", "write-without-response");
		if (res < 0) {
			return res;
		}
	}

	if (flags & GATT_FLAG_NOTIFY) {
		res = sd_bus_message_append(reply, "s", "notify");
		if (res < 0) {
			return res;
		}
	}

	return sd_bus_message_close_container(reply);
}

static int read_service_path(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userdata, sd_bus_error*) {
	struct GattCharacteristic* characteristic = userdata;
	struct GattService* service = characteristic->service;
	return sd_bus_message_append(reply, "o", service->path);
}

static int read_characteristic_path(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userdata, sd_bus_error*) {
	struct GattDescriptor* descriptor = userdata;
	struct GattCharacteristic* characteristic = descriptor->characteristic;
	struct GattService* service = characteristic->service;
	char path[PATH_MAX] = {0};
	size_t i;
	for (i = 0; i < service->nCharacteristics; ++i) {
		if (characteristic == service->characteristics[i]) {
			break;
		}
	}
	snprintf(path, sizeof(path) - 1, "%s/char%04zx", service->path, i);
	return sd_bus_message_append(reply, "o", path);
}

void gatt_service_create(struct GattService* service, const char* uuid, const char* path) {
	memset(service, 0, sizeof(*service));
	strncpy(service->uuid, uuid, sizeof(service->uuid) - 1);
	service->primary = true;
	service->path = strdup(path);
}

void gatt_service_destroy(struct GattService* service) {
	size_t i, j;
	for (i = 0; i < service->nCharacteristics; ++i) {
		struct GattCharacteristic* characteristic = service->characteristics[i];
		sd_bus_slot_unref(characteristic->slot);
		for (j = 0; j < characteristic->nDescriptors; ++j) {
			sd_bus_slot_unref(characteristic->descriptors[j]->slot);
		}
	}
	sd_bus_slot_unref(service->slot);
	free(service->path);
}

int gatt_service_register(struct GattService* service, sd_bus* bus) {
	size_t i, j;
	int res = sd_bus_add_object_vtable(bus, &service->slot, service->path,
		"org.bluez.GattService1", gatt_service, service);
	if (res < 0) {
		return res;
	}

	for (i = 0; i < service->nCharacteristics; ++i) {
		char path[PATH_MAX] = {0};
		struct GattCharacteristic* characteristic = service->characteristics[i];
		snprintf(path, sizeof(path) - 1, "%s/char%04zx", service->path, i);
		res = sd_bus_add_object_vtable(bus, &characteristic->slot, path,
			 "org.bluez.GattCharacteristic1", gatt_characteristic, characteristic);
		if (res < 0) {
			return res;
		}
		for (j = 0; j < characteristic->nDescriptors; ++j) {
			snprintf(path, sizeof(path) - 1, "%s/char%04zx/desc%04zx", service->path, i, j);
			res = sd_bus_add_object_vtable(bus, &characteristic->descriptors[j]->slot, path,
				 "org.bluez.GattDescriptor1", gatt_descriptor, characteristic->descriptors[j]);
			if (res < 0) {
				return res;
			}
		}
	}
	return 0;
};

void gatt_characteristic_create(struct GattCharacteristic* characteristic, const char* uuid, struct GattService* parent) {
	memset(characteristic, 0, sizeof(*characteristic));
	strncpy(characteristic->uuid, uuid, sizeof(characteristic->uuid) - 1);
	characteristic->service = parent;

	if (parent->nCharacteristics == MAX_GATT_CHAR) {
		abort();
	}

	parent->characteristics[parent->nCharacteristics] = characteristic;
	++parent->nCharacteristics;
}

void gatt_descriptor_create(struct GattDescriptor* descriptor, const char* uuid, struct GattCharacteristic* parent) {
	memset(descriptor, 0, sizeof(*descriptor));
	strncpy(descriptor->uuid, uuid, sizeof(descriptor->uuid) - 1);
	descriptor->characteristic = parent;

	if (parent->nDescriptors == MAX_GATT_DESC) {
		abort();
	}

	parent->descriptors[parent->nDescriptors] = descriptor;
	++parent->nDescriptors;
}
