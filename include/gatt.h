/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once

#include "dbus.h"

#define MAX_CHAR 8

struct GattCharacteristic {
	char uuid[37];
	const char* service;
	const char** flags;
	uint16_t mtu;
	void* data;
	size_t size;

	sd_bus_slot* slot;
};

struct GattService {
	bool primary;
	char uuid[37];

	struct GattCharacteristic* characteristics[MAX_CHAR];
	size_t nCharacteristics;

	const char* path;
	sd_bus_slot* slot;
};

void gatt_service_create(struct GattService* service, const char* uuid, const char* path);
int gatt_service_register(struct GattService* service, sd_bus* bus);

void gatt_characteristic_create(struct GattCharacteristic* characteristic, const char* uuid, struct GattService* parent);
