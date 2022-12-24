/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once

#include <stdbool.h>

struct Buffer {
	void* data;
	size_t size;
};

__attribute__((format(printf, 1, 3))) int vmkdir(const char* pattern, int mode, ...);
__attribute__((format(printf, 1, 4))) int vopen(const char* pattern, int flags, int mode, ...);
bool cp_prop(const char* restrict indir, const char* inpath, const char* restrict outdir, const char* outpath);
bool cp_prop_hex(const char* restrict indir, const char* inpath, const char* restrict outdir, const char* outpath);

void buffer_create(struct Buffer*);
void buffer_destroy(struct Buffer*);
void buffer_alloc(struct Buffer*, size_t size);
void buffer_realloc(struct Buffer*, size_t size);
