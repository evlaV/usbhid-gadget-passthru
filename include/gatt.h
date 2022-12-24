/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once

#include "dbus.h"
#include "util.h"

#define MAX_GATT_CHAR 16
#define MAX_GATT_DESC 16

struct GattService;
struct GattDescriptor;

struct GattDescriptor {
	char uuid[37];
	struct GattCharacteristic* characteristic;
	const char** flags;
	struct Buffer data;

	sd_bus_slot* slot;
};

struct GattCharacteristic {
	char uuid[37];
	struct GattService* service;
	const char** flags;
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

	const char* path;
	sd_bus_slot* slot;
};

void gatt_service_create(struct GattService* service, const char* uuid, const char* path);
int gatt_service_register(struct GattService* service, sd_bus* bus);

void gatt_characteristic_create(struct GattCharacteristic* characteristic, const char* uuid, struct GattService* parent);

void gatt_descriptor_create(struct GattDescriptor* descriptor, const char* uuid, struct GattCharacteristic* parent);
