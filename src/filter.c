/* SPDX-License-Identifier: BSD-3-Clause */
#include "filter.h"

#include <stdlib.h>
#include <string.h>

#define REPORT_SIZE_MAX 512

static const uint8_t digital_mask[64] = {
	0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0xFF, 0xFF, 0xFF, 0xFF,  0xFF, 0xFF, 0xFF, 0xFF,
	0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
};

const struct ReportFilter deck_filter = {
	.report_size = 64,
	.seq_number_offset = 0x4,
	.seq_number_width = 4,
	.priority_mask = digital_mask,
};

bool filter_update(const struct ReportFilter* filter, void* old_data, const void* new_data, size_t size) {
	uint8_t update_buffer[REPORT_SIZE_MAX];
	uint8_t test_buffer[REPORT_SIZE_MAX];
	size_t i;
	bool do_flush = false;

	if (filter->report_size != size) {
		return false;
	}
	if (size > REPORT_SIZE_MAX) {
		abort();
	}

	memcpy(test_buffer, old_data, size);
	memcpy(update_buffer, new_data, size);

	for (i = 0; i < size; i += 4) {
		uint32_t mask = *(const uint32_t*) &filter->priority_mask[i];

		*(uint32_t*) &test_buffer[i] &= mask;
		*(uint32_t*) &update_buffer[i] &= mask;
	}

	do_flush = memcmp(update_buffer, test_buffer, size) != 0;

	for (i = 0; i < size; i += 4) {
		uint32_t mask = *(const uint32_t*) &filter->priority_mask[i];	

		((uint32_t*) old_data)[i / 4] = *(uint32_t*) &update_buffer[i] & mask;
		((uint32_t*) old_data)[i / 4] |= ((const uint32_t*) new_data)[i / 4] & ~mask;
	}

	return do_flush;
}
