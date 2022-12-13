/* SPDX-License-Identifier: BSD-3-Clause */
#include <stdbool.h>
#include <stddef.h>

bool find_function(const char* syspath, char* function, size_t function_size);
int find_dev_node(unsigned nod_major, unsigned nod_minor, const char* prefix);
int find_dev(const char* file, const char* class);
bool find_dev_by_id(const char* vidpid, char* out);
int find_hidraw(const char* syspath);
