/*
 * xxhash.c — Single-file compilation unit for xxHash library.
 * Defines XXH_STATIC_LINKING_ONLY and XXH_IMPLEMENTATION so the
 * function bodies and struct definitions are compiled here.
 * All other files include "xxhash.h" normally (declarations only).
 */
#define XXH_STATIC_LINKING_ONLY
#define XXH_IMPLEMENTATION
#include "xxhash.h"
