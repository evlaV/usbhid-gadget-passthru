/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once

#include "dbus.h"
#include "util.h"

#define MAX_GATT_CHAR 16
#define MAX_GATT_DESC 16

struct GattService;
struct GattDescriptor;

enum {
	GATT_FLAG_READ = 1,
	GATT_FLAG_WRITE = 2,
	GATT_FLAG_RW = 3,
	GATT_FLAG_NOTIFY = 4,
	GATT_FLAG_WRITE_NO_RESPONSE = 8,
};

struct GattDescriptor {
	char uuid[37];
	struct GattCharacteristic* characteristic;
	uint32_t flags;
	struct Buffer data;

	sd_bus_slot* slot;
};

struct GattCharacteristic {
	char uuid[37];
	struct GattService* service;
	uint32_t flags;
	uint16_t mtu;
	struct Buffer data;

	struct GattDescriptor* descriptors[MAX_GATT_DESC];
	size_t nDescriptors;

	sd_bus_slot* slot;
};

struct GattService {
	bool primary;
	char uuid[37];

	struct GattCharacteristic* characteristics[MAX_GATT_CHAR];
	size_t nCharacteristics;

	char* path;
	sd_bus_slot* slot;
};

void gatt_service_create(struct GattService* service, const char* uuid, const char* path);
void gatt_service_destroy(struct GattService* service);
int gatt_service_register(struct GattService* service, sd_bus* bus);

void gatt_characteristic_create(struct GattCharacteristic* characteristic, const char* uuid, struct GattService* parent);

void gatt_descriptor_create(struct GattDescriptor* descriptor, const char* uuid, struct GattCharacteristic* parent);
