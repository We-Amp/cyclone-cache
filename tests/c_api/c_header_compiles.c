/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2024-2026 We-Amp B.V. */

/*
 * Regression guard: <cyclone/cyclone_c.h> is the C ABI header, so it must
 * compile as plain C11 -- it once used C++ alias declarations ("using X =
 * ...") and did not.  This translation unit is built with C_STANDARD 11 as
 * part of the test build; it exists to be COMPILED.  It only touches the
 * header's types and declarations and is never called at run time.
 */

#include <cyclone/cyclone_c.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * The error and tier types must stay one byte wide so the C and C++ ABIs
 * agree, and their constants must be usable in constant expressions.
 */
_Static_assert(sizeof(CycloneError) == 1, "CycloneError must be one byte");
_Static_assert(sizeof(CycloneTier) == 1, "CycloneTier must be one byte");
_Static_assert(CYCLONE_OK == 0, "CYCLONE_OK must be zero");
_Static_assert(CYCLONE_TIER_DEFAULT == 0, "CYCLONE_TIER_DEFAULT must be zero");

/* The callback typedefs must be assignable from a matching C function. */
static void cyclone_c_on_read(void *user_data, const char *data,
                              size_t data_len, CycloneError err) {
  (void)user_data;
  (void)data;
  (void)data_len;
  (void)err;
}

/* Declared in this TU only; exercised by the compiler, not by any test. */
CycloneError cyclone_c_header_compiles(void);

CycloneError cyclone_c_header_compiles(void) {
  CycloneCacheConfig config;
  CycloneCacheStats stats;
  CycloneCacheHandle *cache = NULL; /* opaque handle, incomplete type */
  CycloneReadHandle *read_handle = NULL;
  CycloneReadCallback read_cb = cyclone_c_on_read;
  CycloneMissDoneCallback miss_done_cb = cyclone_c_on_read;
  CycloneMissHandler miss_handler = NULL;
  CycloneTier tier = CYCLONE_TIER_SMALL;

  memset(&config, 0, sizeof(config));
  memset(&stats, 0, sizeof(stats));
  config.cache_path = "/nonexistent";
  config.cache_size_bytes = 1;
  config.max_object_size = UINT64_MAX;

  if (cache != NULL || read_handle != NULL || read_cb == NULL ||
      miss_done_cb == NULL || miss_handler != NULL) {
    return CYCLONE_INTERNAL_ERROR;
  }
  if (tier != CYCLONE_TIER_SMALL || stats.ram_cache_hits != 0) {
    return CYCLONE_INTERNAL_ERROR;
  }
  return CYCLONE_OK;
}
