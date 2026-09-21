// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The plain-C seam between the CUDA benchmark's two translation units.
//
// Cyclone's public headers are C++23 (std::expected), and nvcc 12.4 tops out
// at C++20 -- so the store logic cannot live in a .cu.  benchmarks/
// kv_gpu_cuda_host.cpp holds everything that touches Cyclone, LMDB or the
// workload and is compiled as C++23 by the host compiler; benchmarks/
// kv_gpu_cuda.cu holds nothing but CUDA runtime calls and is compiled as
// C++20 by nvcc.  This header is the whole contract between them.
//
// Every entry point returns 0 on success or a cudaError_t rendered as an int;
// cygpu_error_string() turns one back into the CUDA spelling.

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// What the host side prints in its machine-info block.  Plain C so the .cu
// can fill it without seeing a C++ type.
typedef struct CyGpuDeviceInfo {
  char name[256];
  int cc_major;
  int cc_minor;
  int driver_version;   // cudaDriverGetVersion
  int runtime_version;  // cudaRuntimeGetVersion
  int pci_domain_id;
  int pci_bus_id;
  int pci_device_id;
  int unified_addressing;
  int can_map_host_memory;
  int multi_processor_count;
  int memory_bus_width;
  int memory_clock_khz;
  unsigned long long total_global_mem;
} CyGpuDeviceInfo;

// Selects device 0 and creates the single stream every async path uses.
int cygpu_init(void);
void cygpu_shutdown(void);
const char *cygpu_error_string(int err);
int cygpu_device_info(CyGpuDeviceInfo *out);

// One device buffer of `device_bytes` (the transfer destination, addressed by
// slot offset) and one cudaMallocHost buffer of `pinned_bytes` (the `staged`
// path's source and the PCIe ceiling reference's source).
int cygpu_alloc_buffers(size_t device_bytes, size_t pinned_bytes);
void cygpu_free_buffers(void);
void *cygpu_pinned_ptr(void);

// cudaHostRegister / cudaHostUnregister.  `read_only` adds
// cudaHostRegisterReadOnly to cudaHostRegisterDefault.
int cygpu_host_register(const void *base, size_t len, int read_only);
int cygpu_host_unregister(const void *base);

// Transfers into the device buffer at `dst_off`.  `..._sync` is a blocking
// cudaMemcpy (the naive pageable path); `..._async` enqueues on the stream
// and the caller calls cygpu_stream_sync() once per batch.
int cygpu_memcpy_h2d_sync(const void *src, size_t dst_off, size_t len);
int cygpu_memcpy_h2d_async(const void *src, size_t dst_off, size_t len);
int cygpu_stream_sync(void);

// Device-to-host readback, for the end-of-path correctness check.
int cygpu_memcpy_d2h(void *dst, size_t src_off, size_t len);

#ifdef __cplusplus
}  // extern "C"
#endif
