/* SPDX-License-Identifier: BSD-3-Clause */
#include <stdbool.h>

__attribute__((format(printf, 1, 3))) int vmkdir(const char* pattern, int mode, ...);
__attribute__((format(printf, 1, 4))) int vopen(const char* pattern, int flags, int mode, ...);
bool cp_prop(const char* restrict indir, const char* inpath, const char* restrict outdir, const char* outpath);
bool cp_prop_hex(const char* restrict indir, const char* inpath, const char* restrict outdir, const char* outpath);
