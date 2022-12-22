/* SPDX-License-Identifier: BSD-3-Clause */
#include "dbus.h"

int read_bool(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userdata, sd_bus_error*) {
	bool b = *(bool*) userdata;
	return sd_bus_message_append_basic(reply, 'b', &b);
}

int read_uint16(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userdata, sd_bus_error*) {
	uint16_t q = *(uint16_t*) userdata;
	return sd_bus_message_append_basic(reply, 'q', &q);
}

int read_object_indirect(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userdata, sd_bus_error*) {
	const char* direct = *(const char**) userdata;
	return sd_bus_message_append(reply, "o", direct);
}

int read_string(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userdata, sd_bus_error*) {
	return sd_bus_message_append(reply, "s", userdata);
}

int read_string_indirect(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userdata, sd_bus_error*) {
	const char* direct = *(const char**) userdata;
	return sd_bus_message_append(reply, "s", direct);
}

int read_string_array(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userdata, sd_bus_error*) {
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

