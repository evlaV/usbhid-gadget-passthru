/* SPDX-License-Identifier: BSD-3-Clause */
#include <stdbool.h>

bool find_sysfs_path(const char* name, char* syspath, char* bus_id);
int interface_count(const char* syspath);
int interface_type(const char* syspath, const char* bus_id, int interface);
