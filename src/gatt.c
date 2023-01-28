/* SPDX-License-Identifier: BSD-3-Clause */
#include "dbus.h"
#include "gatt.h"

#include <dirent.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

struct Flags {
	size_t offset;
	uint16_t mtu;
	bool reply;
};

static int read_characteristic(sd_bus_message* m, void *userdata, sd_bus_error* error);
static int write_characteristic(sd_bus_message* m, void *userdata, sd_bus_error* error);
static int acquire_notify(sd_bus_message* m, void *userdata, sd_bus_error* error);
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
	SD_BUS_PROPERTY("NotifyAcquired", "b", read_bool, offsetof(struct GattCharacteristic, notify_acquired), 0),
	SD_BUS_METHOD_WITH_ARGS("ReadValue", "a{sv}", "ay", read_characteristic, 0),
	SD_BUS_METHOD_WITH_ARGS("WriteValue", "aya{sv}", SD_BUS_NO_RESULT, write_characteristic, 0),
	SD_BUS_METHOD_WITH_ARGS("AcquireNotify", "a{sv}", "hq", acquire_notify, 0),
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

static int parse_flags(sd_bus_message* m, struct Flags* flags, sd_bus_error*) {
	const char* opt;
	int res;

	res = sd_bus_message_enter_container(m, 'a', "{sv}");
	if (res < 0) {
		fprintf(stderr, "Failed to enter flags container (%i)\n", -res);
		return res;
	}

	while ((res = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		char type;
		res = sd_bus_message_read(m, "s", &opt);
		if (res < 0) {
			fprintf(stderr, "Failed read flag name (%i)\n", -res);
			return res;
		}
		res = sd_bus_message_peek_type(m, &type, NULL);
		if (res < 0) {
			return res;
		}
		if (strcasecmp(opt, "offset") == 0) {
			if (type == 'q') {
				res = sd_bus_message_read(m, "q", &flags->offset);
			} else if (type == 'v') {
				res = sd_bus_message_read(m, "v", "q", &flags->offset);
			} else {
				return -EINVAL;
			}
			if (res < 0) {
				fprintf(stderr, "Failed read offset value (%i)\n", -res);
				return res;
			}
		} else if (strcasecmp(opt, "mtu") == 0) {
			if (type == 'q') {
				res = sd_bus_message_read(m, "q", &flags->mtu);
			} else if (type == 'v') {
				res = sd_bus_message_read(m, "v", "q", &flags->mtu);
			} else {
				return -EINVAL;
			}
			if (res < 0) {
				fprintf(stderr, "Failed read mtu value (%i)\n", -res);
				return res;
			}
		} else if (strcasecmp(opt, "type") == 0) {
			const char* typename;
			if (type == 's') {
				res = sd_bus_message_read(m, "s", &typename);
			} else if (type == 's') {
				res = sd_bus_message_read(m, "v", "s", &typename);
			} else {
				return -EINVAL;
			}
			if (res < 0) {
				fprintf(stderr, "Failed read type value (%i)\n", -res);
				return res;
			}
			if (strcasecmp(typename, "command") == 0) {
				flags->reply = false;
			} else if (strcasecmp(typename, "request") == 0) {
				flags->reply = true;
			}
		} else if (strcasecmp(opt, "link") == 0) {
			/* We don't care about this message */
			sd_bus_message_skip(m, NULL);
		} else {
			fprintf(stderr, "Unhandled flag: %s : %c\n", opt, type);
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
	return 0;
}

static int read_characteristic(sd_bus_message* m, void *userdata, sd_bus_error* error) {
	struct GattCharacteristic* characteristic = userdata;
	int res;
	struct Flags flags = {.mtu = 517};
	size_t length = characteristic->data.size;
	sd_bus_message* reply;

	if (!(characteristic->flags & GATT_FLAG_READ)) {
		fprintf(stderr, "Disallowed read to write-only characteristic (%s)\n", characteristic->uuid);
		return sd_bus_error_set(error, "org.bluez.Error.NotSupported", "Reading not supported");
	}

	res = parse_flags(m, &flags, error);
	if (res < 0 || sd_bus_error_is_set(error)) {
		fprintf(stderr, "Failed to parse flags for characteristic read (%i)\n", -res);
		return res;
	}

	if (flags.offset > length) {
		fprintf(stderr, "Invalid characteristic offset (%zu > %zu)\n", flags.offset, length);
		return sd_bus_error_setf(error, "org.bluez.Error.InvalidOffset",
			"Requested offset %zu exceeds characteristic size %zu", flags.offset, length);
	}
	length -= flags.offset;
	if (flags.mtu && length > flags.mtu) {
		length = flags.mtu;
	}

	res = sd_bus_message_new_method_return(m, &reply);
	if (res < 0) {
		return res;
	}

	res = sd_bus_message_append_array(reply, 'y', &((uint8_t*) characteristic->data.data)[flags.offset], length);
	if (res < 0) {
		return res;
	}

	return sd_bus_message_send(reply);
}

static int write_characteristic(sd_bus_message* m, void *userdata, sd_bus_error* error) {
	struct GattCharacteristic* characteristic = userdata;
	int res;
	struct Flags flags = {
		.reply = true,
		.mtu = 517
	};
	size_t size;
	const void* data;

	if (!(characteristic->flags & GATT_FLAG_WRITE)) {
		fprintf(stderr, "Disallowed write to read-only characteristic (%s)\n", characteristic->uuid);
		return sd_bus_error_set(error, "org.bluez.Error.NotSupported", "Writing not supported");
	}

	res = sd_bus_message_read_array(m, 'y', &data, &size);
	if (res < 0) {
		fprintf(stderr, "Failed to read data for characteristic write (%i)\n", -res);
		return res;
	}

	res = parse_flags(m, &flags, error);
	if (res < 0 || sd_bus_error_is_set(error)) {
		fprintf(stderr, "Failed to parse flags for characteristic wr (%i)\n", -res);
		return res;
	}

	if (!flags.reply && !(characteristic->flags & GATT_FLAG_WRITE_NO_RESPONSE)) {
		fprintf(stderr, "Disallowed write without response (%s)\n", characteristic->uuid);
		return sd_bus_error_set(error, "org.bluez.Error.NotSupported", "Writing without response not supported");
	}

	res = characteristic->write(data, size, flags.offset, flags.mtu, characteristic->userdata);
	if (res < 0) {
		return res;
	}
	if (flags.reply) {
		return sd_bus_reply_method_return(m, NULL);
	}
	return 0;
}

static int acquire_notify(sd_bus_message* m, void *userdata, sd_bus_error* error) {
	struct GattCharacteristic* characteristic = userdata;
	int res;
	struct Flags flags = {.mtu = 517};
	int fds[2];
	sd_bus_message* reply;

	if (!(characteristic->flags & GATT_FLAG_READ)) {
		fprintf(stderr, "Disallowed acquire-notify for write-only characteristic (%s)\n", characteristic->uuid);
		return sd_bus_error_set(error, "org.bluez.Error.NotSupported", "Reading not supported");
	}

	if (characteristic->notify_acquired) {
		return sd_bus_error_set(error, "org.bluez.Error.Failed", "Notify already acquired");
	}

	res = parse_flags(m, &flags, error);
	if (res < 0 || sd_bus_error_is_set(error)) {
		return res;
	}

	res = socketpair(AF_LOCAL, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, fds);
	if (res < 0) {
		return res;
	}

	characteristic->notify_fd = fds[1];
	characteristic->notify_acquired = true;

	res = sd_bus_message_new_method_return(m, &reply);
	if (res < 0) {
		return res;
	}

	res = sd_bus_message_append_basic(reply, 'h', &fds[0]);
	if (res < 0) {
		return res;
	}

	res = sd_bus_message_append_basic(reply, 'q', &flags.mtu);
	if (res < 0) {
		return res;
	}
	return sd_bus_message_send(reply);
}

static int read_descriptor(sd_bus_message* m, void *userdata, sd_bus_error* error) {
	struct GattDescriptor* descriptor = userdata;
	int res;
	struct Flags flags = {.mtu = 517};
	size_t length = descriptor->data.size;
	sd_bus_message* reply;

	if (!(descriptor->flags & GATT_FLAG_READ)) {
		fprintf(stderr, "Disallowed read from write-only descriptor (%s)\n", descriptor->uuid);
		return sd_bus_error_set(error, "org.bluez.Error.NotSupported", "Reading not supported");
	}

	res = parse_flags(m, &flags, error);
	if (res < 0 || sd_bus_error_is_set(error)) {
		fprintf(stderr, "Failed to parse flags for descriptor read (%i)\n", -res);
		return res;
	}

	if (flags.offset > length) {
		fprintf(stderr, "Invalid descriptor offset (%zu > %zu)\n", flags.offset, length);
		return sd_bus_error_setf(error, "org.bluez.Error.InvalidOffset",
			"Requested offset %lu exceeds descriptor size %zu", flags.offset, length);
	}
	length -= flags.offset;
	if (flags.mtu && length > flags.mtu) {
		length = flags.mtu;
	}

	res = sd_bus_message_new_method_return(m, &reply);
	if (res < 0) {
		return res;
	}

	res = sd_bus_message_append_array(reply, 'y', &((uint8_t*) descriptor->data.data)[flags.offset], length);
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
		if (characteristic->notify_fd >= 0) {
			close(characteristic->notify_fd);
		}
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
	characteristic->notify_fd = -1;
	characteristic->mtu = 517;

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
