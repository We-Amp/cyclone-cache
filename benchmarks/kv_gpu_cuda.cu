// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// CUDA half of the host-to-device transfer experiment: nothing but runtime
// API calls behind the plain-C seam in benchmarks/kv_gpu_cuda.h.
//
// This file is deliberately boring.  It has no kernels, no Cyclone headers
// and no C++23 -- nvcc 12.4 stops at C++20, and cyclone/handle.hpp needs
// std::expected, so every line that knows what a cache is lives in
// benchmarks/kv_gpu_cuda_host.cpp instead.  Keeping the split at "CUDA calls
// vs everything else" is what lets the .cu compile under nvcc while the
// benchmark still links against the real library.
//
// One stream, one device buffer, one pinned buffer: the batched paths enqueue
// N cudaMemcpyAsync on that stream and synchronize once, which is exactly the
// batching the experiment is measuring.

#include <cuda_runtime.h>

#include <cstring>

#include "kv_gpu_cuda.h"

namespace {

cudaStream_t g_stream = nullptr;
void *g_device = nullptr;
void *g_pinned = nullptr;
size_t g_device_bytes = 0;
size_t g_pinned_bytes = 0;

// Every transfer lands inside the one device allocation; a slot offset past
// its end would be a silent out-of-bounds DMA, so it is checked.
bool device_range_ok(size_t off, size_t len) {
  return g_device != nullptr && off <= g_device_bytes &&
         len <= g_device_bytes - off;
}

}  // namespace

extern "C" {

int cygpu_init(void) {
  cudaError_t err = cudaSetDevice(0);
  if (err != cudaSuccess) return static_cast<int>(err);
  // Establish the context now rather than inside the first timed unit.
  err = cudaFree(nullptr);
  if (err != cudaSuccess) return static_cast<int>(err);
  err = cudaStreamCreate(&g_stream);
  return static_cast<int>(err);
}

void cygpu_shutdown(void) {
  cygpu_free_buffers();
  if (g_stream != nullptr) {
    cudaStreamDestroy(g_stream);
    g_stream = nullptr;
  }
  cudaDeviceReset();
}

const char *cygpu_error_string(int err) {
  return cudaGetErrorString(static_cast<cudaError_t>(err));
}

int cygpu_device_info(CyGpuDeviceInfo *out) {
  if (out == nullptr) return static_cast<int>(cudaErrorInvalidValue);
  std::memset(out, 0, sizeof(*out));

  cudaDeviceProp prop{};
  cudaError_t err = cudaGetDeviceProperties(&prop, 0);
  if (err != cudaSuccess) return static_cast<int>(err);

  std::strncpy(out->name, prop.name, sizeof(out->name) - 1);
  out->cc_major = prop.major;
  out->cc_minor = prop.minor;
  out->pci_domain_id = prop.pciDomainID;
  out->pci_bus_id = prop.pciBusID;
  out->pci_device_id = prop.pciDeviceID;
  out->unified_addressing = prop.unifiedAddressing;
  out->can_map_host_memory = prop.canMapHostMemory;
  out->multi_processor_count = prop.multiProcessorCount;
  out->memory_bus_width = prop.memoryBusWidth;
  out->memory_clock_khz = prop.memoryClockRate;
  out->total_global_mem = static_cast<unsigned long long>(prop.totalGlobalMem);

  cudaDriverGetVersion(&out->driver_version);
  cudaRuntimeGetVersion(&out->runtime_version);
  return 0;
}

int cygpu_alloc_buffers(size_t device_bytes, size_t pinned_bytes) {
  cygpu_free_buffers();
  cudaError_t err = cudaMalloc(&g_device, device_bytes);
  if (err != cudaSuccess) {
    g_device = nullptr;
    return static_cast<int>(err);
  }
  g_device_bytes = device_bytes;
  err = cudaMallocHost(&g_pinned, pinned_bytes);
  if (err != cudaSuccess) {
    g_pinned = nullptr;
    return static_cast<int>(err);
  }
  g_pinned_bytes = pinned_bytes;
  std::memset(g_pinned, 0, g_pinned_bytes);
  return 0;
}

void cygpu_free_buffers(void) {
  if (g_pinned != nullptr) {
    cudaFreeHost(g_pinned);
    g_pinned = nullptr;
    g_pinned_bytes = 0;
  }
  if (g_device != nullptr) {
    cudaFree(g_device);
    g_device = nullptr;
    g_device_bytes = 0;
  }
}

void *cygpu_pinned_ptr(void) { return g_pinned; }

int cygpu_host_register(const void *base, size_t len, int read_only) {
  unsigned int flags = cudaHostRegisterDefault;
#ifdef cudaHostRegisterReadOnly
  if (read_only != 0) flags |= cudaHostRegisterReadOnly;
#else
  (void)read_only;
#endif
  return static_cast<int>(
      cudaHostRegister(const_cast<void *>(base), len, flags));
}

int cygpu_host_unregister(const void *base) {
  return static_cast<int>(cudaHostUnregister(const_cast<void *>(base)));
}

int cygpu_memcpy_h2d_sync(const void *src, size_t dst_off, size_t len) {
  if (!device_range_ok(dst_off, len)) {
    return static_cast<int>(cudaErrorInvalidValue);
  }
  return static_cast<int>(cudaMemcpy(static_cast<char *>(g_device) + dst_off,
                                     src, len, cudaMemcpyHostToDevice));
}

int cygpu_memcpy_h2d_async(const void *src, size_t dst_off, size_t len) {
  if (!device_range_ok(dst_off, len)) {
    return static_cast<int>(cudaErrorInvalidValue);
  }
  return static_cast<int>(
      cudaMemcpyAsync(static_cast<char *>(g_device) + dst_off, src, len,
                      cudaMemcpyHostToDevice, g_stream));
}

int cygpu_stream_sync(void) {
  return static_cast<int>(cudaStreamSynchronize(g_stream));
}

int cygpu_memcpy_d2h(void *dst, size_t src_off, size_t len) {
  if (!device_range_ok(src_off, len)) {
    return static_cast<int>(cudaErrorInvalidValue);
  }
  return static_cast<int>(
      cudaMemcpy(dst, static_cast<const char *>(g_device) + src_off, len,
                 cudaMemcpyDeviceToHost));
}

}  // extern "C"
