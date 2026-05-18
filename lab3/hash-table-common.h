#pragma once

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

#define HASH_TABLE_CAPACITY 4096

uint32_t bernstein_hash(const char *string);
