/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VID_VALVE 0x28de
#define PID_STEAM_DECK 0x1205
#define DECK_RAW_IFACE 2

struct ReportFilter {
	size_t report_size;
	size_t seq_number_offset;
	unsigned seq_number_width;
	const uint8_t* priority_mask;
};

extern const struct ReportFilter deck_filter;

bool filter_update(const struct ReportFilter* filter, void* old_data, const void* new_data, size_t size);
