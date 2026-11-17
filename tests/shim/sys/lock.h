/* check-host shim for devkitPro newlib's <sys/lock.h> (only the two types libnx headers use).
 * Layout mirrors devkitPro newlib: a 32-bit lock word, and a recursive lock with owner tag and counter. */
#pragma once
#include <stdint.h>

typedef uint32_t _LOCK_T;

typedef struct {
    _LOCK_T lock;
    uint32_t thread_tag;
    uint32_t counter;
} _LOCK_RECURSIVE_T;
