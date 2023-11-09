/* SPDX-License-Identifier: BSD-3-Clause */
#include "filter.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

#define REPORT_SIZE_MAX 512UL

static const uint8_t digital_mask[64] = {
	0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0xFF, 0xFF, 0xFF, 0xFF,  0xFF, 0xFF, 0xFF, 0xFF,
	0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
};

static const struct AnalogMask analog_mask[] = {
	{ 16, 2, 0x100 },
	{ 18, 2, 0x100 },
	{ 20, 2, 0x100 },
	{ 22, 2, 0x100 },
	{ 44, 2, 0x200 },
	{ 46, 2, 0x200 },
	{ 48, 2, 0x300 },
	{ 50, 2, 0x300 },
	{ 52, 2, 0x300 },
	{ 54, 2, 0x300 },
	{}
};

const struct ReportFilter deck_filter = {
	.report_size = 64,
	.seq_number_offset = 0x4,
	.seq_number_width = 4,
	.priority_mask = digital_mask,
	.analog_mask = analog_mask,
};

bool filter_update(const struct ReportFilter* filter, const void* old_data, const void* new_data, size_t size) {
	uint8_t update_buffer[REPORT_SIZE_MAX];
	uint8_t test_buffer[REPORT_SIZE_MAX];
	size_t i;
	bool do_flush = false;

	if (filter->report_size != size) {
		return false;
	}
	if (size > REPORT_SIZE_MAX) {
		log_fmt(ERROR, "Report size it too large: %zu > %zu\n", size, REPORT_SIZE_MAX);
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

	if (do_flush) {
		log_fmt(DEBUG, "Mask difference triggering flush\n");
	}

	if (!do_flush) {
		for (i = 0; filter->analog_mask[i].width; ++i) {
			uint64_t old_value = 0;
			uint64_t new_value = 0;
			switch (filter->analog_mask[i].width) {
			case 1:
				old_value = *(uint8_t*) ((uintptr_t) old_data + filter->analog_mask[i].start);
				new_value = *(uint8_t*) ((uintptr_t) new_data + filter->analog_mask[i].start);
				break;
			case 2:
				old_value = *(uint16_t*) ((uintptr_t) old_data + filter->analog_mask[i].start);
				new_value = *(uint16_t*) ((uintptr_t) new_data + filter->analog_mask[i].start);
				break;
			case 4:
				old_value = *(uint32_t*) ((uintptr_t) old_data + filter->analog_mask[i].start);
				new_value = *(uint32_t*) ((uintptr_t) new_data + filter->analog_mask[i].start);
				break;
			case 8:
				old_value = *(uint64_t*) ((uintptr_t) old_data + filter->analog_mask[i].start);
				new_value = *(uint64_t*) ((uintptr_t) new_data + filter->analog_mask[i].start);
				break;
			}

			if (old_value < new_value) {
				do_flush = new_value - old_value >= filter->analog_mask[i].threshold;
			} else {
				do_flush = old_value - new_value >= filter->analog_mask[i].threshold;
			}
			if (do_flush) {
				break;
			}
		}
		if (do_flush) {
			log_fmt(DEBUG, "Analog difference triggering flush\n");
		}
	}

	return do_flush;
}
