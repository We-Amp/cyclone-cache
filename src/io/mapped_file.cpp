// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "mapped_file.hpp"

#include <cerrno>
#include <climits>

#include "cyclone/detail/expected_compat.hpp"

#if defined(CYCLONE_PLATFORM_WINDOWS) || defined(_WIN32)
#define CYCLONE_USE_WIN32_MMAP
#include <windows.h>
#else
#define CYCLONE_USE_POSIX_MMAP
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cyclone {

#ifdef CYCLONE_USE_POSIX_MMAP

class PosixMappedFile : public MappedFile {
 public:
  PosixMappedFile() = default;
  ~PosixMappedFile() override { close(); }

  std::expected<void, std::error_code> open(const std::string &path,
                                            OpenMode mode) override {
    if (_fd >= 0) {
      return make_unexpected(
          std::make_error_code(std::errc::device_or_resource_busy));
    }

    int flags = O_RDONLY;
    int perms = 0;

    switch (mode) {
      case OpenMode::ReadOnly:
        flags = O_RDONLY;
        break;
      case OpenMode::ReadWrite:
        flags = O_RDWR;
        break;
      case OpenMode::Create:
        flags = O_RDWR | O_CREAT;
        perms = 0644;
        break;
    }

    _fd = ::open(path.c_str(), flags, perms);
    if (_fd < 0) {
      return make_unexpected(
          std::make_error_code(static_cast<std::errc>(errno)));
    }

    struct stat st;
    if (fstat(_fd, &st) < 0) {
      ::close(_fd);
      _fd = -1;
      return make_unexpected(
          std::make_error_code(static_cast<std::errc>(errno)));
    }

    _file_size = st.st_size;
    _mode = mode;
    return {};
  }

  void close() override {
    if (_persistent_base != nullptr) {
      munmap(_persistent_base, _persistent_size);
      _persistent_base = nullptr;
      _persistent_size = 0;
    }
    if (_fd >= 0) {
      ::close(_fd);
      _fd = -1;
    }
    _file_size = 0;
  }

  [[nodiscard]] bool is_open() const override { return _fd >= 0; }

  std::expected<void, std::error_code> map_whole_file(MapMode mode) override {
    if (_fd < 0) {
      return make_unexpected(
          std::make_error_code(std::errc::bad_file_descriptor));
    }
    if (_persistent_base != nullptr) {
      return {};  // Already mapped
    }
    if (_file_size == 0) {
      return {};  // Nothing to map
    }

    int prot = PROT_READ;
    if (mode == MapMode::ReadWrite) {
      prot |= PROT_WRITE;
    }

    void *addr = mmap(nullptr, _file_size, prot, MAP_SHARED, _fd, 0);
    if (addr == MAP_FAILED) {
      return make_unexpected(
          std::make_error_code(static_cast<std::errc>(errno)));
    }

    _persistent_base = static_cast<std::byte *>(addr);
    _persistent_size = static_cast<size_t>(_file_size);

#ifdef MADV_RANDOM
    (void)madvise(_persistent_base, _persistent_size, MADV_RANDOM);
#endif
#ifdef MADV_HUGEPAGE
    (void)madvise(_persistent_base, _persistent_size, MADV_HUGEPAGE);
#endif
    return {};
  }

  std::expected<std::span<std::byte>, std::error_code> map_region(
      uint64_t offset, size_t length, MapMode mode) override {
    if (_fd < 0) {
      return make_unexpected(
          std::make_error_code(std::errc::bad_file_descriptor));
    }

    // Fast path: return sub-span of persistent mapping
    if (_persistent_base != nullptr) {
      if (offset > _persistent_size || length > _persistent_size - offset) {
        return make_unexpected(
            std::make_error_code(std::errc::invalid_argument));
      }
      return std::span<std::byte>(_persistent_base + offset, length);
    }

    int prot = PROT_READ;
    int flags = MAP_SHARED;

    if (mode == MapMode::ReadWrite) {
      prot |= PROT_WRITE;
    }

    // mmap requires page-aligned offset
    long page_size = sysconf(_SC_PAGESIZE);
    uint64_t page_offset =
        offset & ~(page_size - 1);  // Round down to page boundary
    size_t offset_adjust =
        offset - page_offset;  // Bytes to skip in mapped region
    size_t map_length = length + offset_adjust;  // Total bytes to map

    void *addr = mmap(nullptr, map_length, prot, flags, _fd, page_offset);
    if (addr == MAP_FAILED) {
      return make_unexpected(
          std::make_error_code(static_cast<std::errc>(errno)));
    }

    // Return span starting at the actual requested offset
    std::byte *data = static_cast<std::byte *>(addr) + offset_adjust;
    return std::span<std::byte>(data, length);
  }

  std::error_code unmap_region(std::span<std::byte> region) override {
    // No-op when persistent mapping is active — the whole file stays mapped
    if (_persistent_base != nullptr) {
      return {};
    }

    // The region might start in the middle of a page-aligned mapping
    // We need to find the actual mapping start (page-aligned)
    long page_size = sysconf(_SC_PAGESIZE);
    auto addr = reinterpret_cast<uintptr_t>(region.data());
    uintptr_t page_addr = addr & ~(page_size - 1);
    size_t offset_adjust = addr - page_addr;
    size_t map_length = region.size() + offset_adjust;

    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    if (munmap(reinterpret_cast<void *>(page_addr), map_length) < 0) {
      return std::make_error_code(static_cast<std::errc>(errno));
    }
    return {};
  }

  std::error_code sync(std::span<std::byte> region, SyncMode mode) override {
    int flags = (mode == SyncMode::Sync) ? MS_SYNC : MS_ASYNC;

    // msync() requires a page-aligned address.  Regions handed out by
    // map_region() may start mid-page (e.g. a directory region at file
    // offset 64), so round the start down to the page boundary — this
    // stays within the enclosing mapping, mirroring unmap_region().
    long page_size = sysconf(_SC_PAGESIZE);
    auto addr = reinterpret_cast<uintptr_t>(region.data());
    uintptr_t page_addr = addr & ~(page_size - 1);
    size_t sync_length = region.size() + (addr - page_addr);

    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    if (msync(reinterpret_cast<void *>(page_addr), sync_length, flags) < 0) {
      return std::make_error_code(static_cast<std::errc>(errno));
    }
    return {};
  }

  std::error_code advise_sequential(std::span<std::byte> region) override {
#ifdef MADV_SEQUENTIAL
    if (madvise(region.data(), region.size(), MADV_SEQUENTIAL) < 0) {
      return std::make_error_code(static_cast<std::errc>(errno));
    }
#else
    (void)region;
#endif
    return {};
  }

  std::error_code advise_random(std::span<std::byte> region) override {
#ifdef MADV_RANDOM
    if (madvise(region.data(), region.size(), MADV_RANDOM) < 0) {
      return std::make_error_code(static_cast<std::errc>(errno));
    }
#else
    (void)region;
#endif
    return {};
  }

  std::error_code advise_willneed(std::span<std::byte> region) override {
#ifdef MADV_WILLNEED
    // madvise() requires a page-aligned start address.  Regions handed out
    // by map_region() may start mid-page (a document begins at an arbitrary
    // file offset), so round the start down to the page boundary and extend
    // the length to match — this stays within the enclosing mapping,
    // mirroring sync() and unmap_region().  Without this the call fails with
    // EINVAL and the readahead silently never happens.
    long page_size = sysconf(_SC_PAGESIZE);
    auto addr = reinterpret_cast<uintptr_t>(region.data());
    uintptr_t page_addr = addr & ~(page_size - 1);
    size_t advise_length = region.size() + (addr - page_addr);

    // Issue the advice in bounded chunks.  Linux clamps ONE MADV_WILLNEED
    // to max(bdi->io_pages, ra_pages) pages (force_page_cache_ra()), so a
    // single call over a multi-megabyte range silently reads ahead only the
    // first ~1 MB of it and the rest still faults in one page at a time.
    // Each call gets its own budget, so chunking is what actually covers
    // the whole document; the extra syscalls are amortised by the caller,
    // which advises a given document once (Volume::maybe_advise_readahead).
    constexpr size_t kChunkBytes = 512 * 1024;
    while (advise_length > 0) {
      size_t chunk = advise_length < kChunkBytes ? advise_length : kChunkBytes;
      // NOLINTNEXTLINE(performance-no-int-to-ptr)
      if (madvise(reinterpret_cast<void *>(page_addr), chunk, MADV_WILLNEED) <
          0) {
        return std::make_error_code(static_cast<std::errc>(errno));
      }
      page_addr += chunk;
      advise_length -= chunk;
    }
#else
    (void)region;
#endif
    return {};
  }

  std::error_code advise_dontneed(std::span<std::byte> region) override {
#ifdef MADV_DONTNEED
    if (madvise(region.data(), region.size(), MADV_DONTNEED) < 0) {
      return std::make_error_code(static_cast<std::errc>(errno));
    }
#else
    (void)region;
#endif
    return {};
  }

  std::error_code advise_readahead(uint64_t file_offset,
                                   size_t length) override {
#if defined(F_RDADVISE)
    if (_fd < 0) {
      return std::make_error_code(std::errc::bad_file_descriptor);
    }
    if (length == 0) {
      return {};
    }
    // Darwin's asynchronous readahead: it queues a clustered read of the
    // range into the unified buffer cache and returns immediately, so a
    // subsequent fault on the shared mapping is a minor fault instead of a
    // serial 4 KiB major fault.  ra_count is an int, so a range larger than
    // INT_MAX is clamped; that only shortens the hint.
    struct radvisory ra;
    ra.ra_offset = static_cast<off_t>(file_offset);
    ra.ra_count = length > static_cast<size_t>(INT_MAX)
                      ? INT_MAX
                      : static_cast<int>(length);
    if (fcntl(_fd, F_RDADVISE, &ra) < 0) {
      return std::make_error_code(static_cast<std::errc>(errno));
    }
    return {};
#else
    (void)file_offset;
    (void)length;
    return std::make_error_code(std::errc::not_supported);
#endif
  }

  [[nodiscard]] bool supports_advise_readahead() const noexcept override {
#if defined(F_RDADVISE)
    return true;
#else
    return false;
#endif
  }

  [[nodiscard]] uint64_t file_size() const override { return _file_size; }

  [[nodiscard]] const std::byte *persistent_base() const override {
    return _persistent_base;
  }
  [[nodiscard]] intptr_t native_fd() const override { return _fd; }

 private:
  int _fd = -1;
  uint64_t _file_size = 0;
  OpenMode _mode = OpenMode::ReadOnly;
  std::byte *_persistent_base = nullptr;
  size_t _persistent_size = 0;
};

#endif  // CYCLONE_USE_POSIX_MMAP

#ifdef CYCLONE_USE_WIN32_MMAP

// Cached allocation granularity (constant for process lifetime).
static DWORD win32_allocation_granularity() {
  static DWORD granularity = [] {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwAllocationGranularity;
  }();
  return granularity;
}

class Win32MappedFile : public MappedFile {
 public:
  Win32MappedFile() = default;
  ~Win32MappedFile() override { close(); }

  std::expected<void, std::error_code> open(const std::string &path,
                                            OpenMode mode) override {
    if (_file_handle != INVALID_HANDLE_VALUE) {
      return make_unexpected(
          std::make_error_code(std::errc::device_or_resource_busy));
    }

    DWORD access = GENERIC_READ;
    DWORD share = FILE_SHARE_READ;
    DWORD creation = OPEN_EXISTING;

    switch (mode) {
      case OpenMode::ReadOnly:
        access = GENERIC_READ;
        share = FILE_SHARE_READ | FILE_SHARE_WRITE;
        creation = OPEN_EXISTING;
        break;
      case OpenMode::ReadWrite:
        access = GENERIC_READ | GENERIC_WRITE;
        share = FILE_SHARE_READ | FILE_SHARE_WRITE;
        creation = OPEN_EXISTING;
        break;
      case OpenMode::Create:
        access = GENERIC_READ | GENERIC_WRITE;
        share = FILE_SHARE_READ | FILE_SHARE_WRITE;
        creation = OPEN_ALWAYS;
        break;
    }

    _file_handle = CreateFileA(path.c_str(), access, share, nullptr, creation,
                               FILE_ATTRIBUTE_NORMAL, nullptr);

    if (_file_handle == INVALID_HANDLE_VALUE) {
      return make_unexpected(
          std::error_code(GetLastError(), std::system_category()));
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(_file_handle, &size)) {
      CloseHandle(_file_handle);
      _file_handle = INVALID_HANDLE_VALUE;
      return make_unexpected(
          std::error_code(GetLastError(), std::system_category()));
    }

    _file_size = size.QuadPart;
    _mode = mode;
    return {};
  }

  void close() override {
    if (_persistent_base != nullptr) {
      UnmapViewOfFile(_persistent_base);
      _persistent_base = nullptr;
      _persistent_size = 0;
    }
    if (_persistent_mapping != INVALID_HANDLE_VALUE) {
      CloseHandle(_persistent_mapping);
      _persistent_mapping = INVALID_HANDLE_VALUE;
    }
    if (_file_handle != INVALID_HANDLE_VALUE) {
      CloseHandle(_file_handle);
      _file_handle = INVALID_HANDLE_VALUE;
    }
    _file_size = 0;
  }

  bool is_open() const override { return _file_handle != INVALID_HANDLE_VALUE; }

  std::expected<void, std::error_code> map_whole_file(MapMode mode) override {
    if (_file_handle == INVALID_HANDLE_VALUE) {
      return make_unexpected(
          std::make_error_code(std::errc::bad_file_descriptor));
    }
    if (_persistent_base != nullptr) {
      return {};  // Already mapped
    }
    if (_file_size == 0) {
      return {};  // Nothing to map
    }

    DWORD protect =
        (mode == MapMode::ReadWrite) ? PAGE_READWRITE : PAGE_READONLY;
    DWORD map_access =
        (mode == MapMode::ReadWrite) ? FILE_MAP_WRITE : FILE_MAP_READ;

    HANDLE mapping =
        CreateFileMappingA(_file_handle, nullptr, protect, 0, 0, nullptr);
    if (mapping == nullptr) {
      return make_unexpected(
          std::error_code(GetLastError(), std::system_category()));
    }

    void *addr = MapViewOfFile(mapping, map_access, 0, 0, 0);
    if (addr == nullptr) {
      DWORD err = GetLastError();
      CloseHandle(mapping);
      return make_unexpected(std::error_code(err, std::system_category()));
    }

    _persistent_base = static_cast<std::byte *>(addr);
    _persistent_size = static_cast<size_t>(_file_size);
    _persistent_mapping = mapping;
    return {};
  }

  std::expected<std::span<std::byte>, std::error_code> map_region(
      uint64_t offset, size_t length, MapMode mode) override {
    if (_file_handle == INVALID_HANDLE_VALUE) {
      return make_unexpected(
          std::make_error_code(std::errc::bad_file_descriptor));
    }

    // Fast path: return sub-span of persistent mapping
    if (_persistent_base != nullptr) {
      if (offset > _persistent_size || length > _persistent_size - offset) {
        return make_unexpected(
            std::make_error_code(std::errc::invalid_argument));
      }
      return std::span<std::byte>(_persistent_base + offset, length);
    }

    DWORD protect =
        (mode == MapMode::ReadWrite) ? PAGE_READWRITE : PAGE_READONLY;
    DWORD map_access =
        (mode == MapMode::ReadWrite) ? FILE_MAP_WRITE : FILE_MAP_READ;

    HANDLE mapping =
        CreateFileMappingA(_file_handle, nullptr, protect, 0, 0, nullptr);
    if (mapping == nullptr) {
      return make_unexpected(
          std::error_code(GetLastError(), std::system_category()));
    }

    // MapViewOfFile requires offset aligned to allocation granularity
    uint64_t granularity = win32_allocation_granularity();
    uint64_t aligned_offset = offset & ~(granularity - 1);
    size_t offset_adjust = static_cast<size_t>(offset - aligned_offset);
    size_t map_length = length + offset_adjust;

    DWORD offset_high = static_cast<DWORD>(aligned_offset >> 32);
    DWORD offset_low = static_cast<DWORD>(aligned_offset & 0xFFFFFFFF);

    void *addr =
        MapViewOfFile(mapping, map_access, offset_high, offset_low, map_length);
    if (addr == nullptr) {
      DWORD err = GetLastError();
      CloseHandle(mapping);
      return make_unexpected(std::error_code(err, std::system_category()));
    }
    CloseHandle(mapping);

    // Return span starting at the actual requested offset
    std::byte *data = static_cast<std::byte *>(addr) + offset_adjust;
    return std::span<std::byte>(data, length);
  }

  std::error_code unmap_region(std::span<std::byte> region) override {
    // No-op when persistent mapping is active — the whole file stays mapped
    if (_persistent_base != nullptr) {
      return {};
    }

    // The region pointer may be offset from the MapViewOfFile base due to
    // alignment adjustment.  MapViewOfFile returns addresses aligned to the
    // allocation granularity, so round down to find the original base.
    uintptr_t addr = reinterpret_cast<uintptr_t>(region.data());
    uintptr_t base_addr =
        addr & ~(static_cast<uintptr_t>(win32_allocation_granularity()) - 1);

    if (!UnmapViewOfFile(reinterpret_cast<void *>(base_addr))) {
      return std::error_code(GetLastError(), std::system_category());
    }
    return {};
  }

  std::error_code sync(std::span<std::byte> region, SyncMode mode) override {
    // FlushViewOfFile initiates a write to the filesystem cache (async).
    if (!FlushViewOfFile(region.data(), region.size())) {
      return std::error_code(GetLastError(), std::system_category());
    }
    // For synchronous mode, also call FlushFileBuffers to ensure data
    // reaches stable storage.  Skip for async to avoid blocking.
    if (mode == SyncMode::Sync && _file_handle != INVALID_HANDLE_VALUE) {
      FlushFileBuffers(_file_handle);
    }
    return {};
  }

  std::error_code advise_sequential(std::span<std::byte> region) override {
    (void)region;
    return {};
  }

  std::error_code advise_random(std::span<std::byte> region) override {
    (void)region;
    return {};
  }

  std::error_code advise_willneed(std::span<std::byte> region) override {
    // Windows equivalent of MADV_WILLNEED: ask the memory manager to fetch
    // the range in as few large I/Os as it can.  PrefetchVirtualMemory has
    // been available since Windows 8 / Server 2012 -- the version guard is
    // for toolchains whose headers still default to an older target, where
    // this degrades to the no-op it was before.  Like the POSIX side this
    // is a pure hint, so a failure is not an error for the caller.
#if defined(_WIN32_WINNT) && _WIN32_WINNT >= 0x0602
    if (region.empty()) {
      return {};
    }
    WIN32_MEMORY_RANGE_ENTRY entry;
    entry.VirtualAddress = region.data();
    entry.NumberOfBytes = region.size();
    if (PrefetchVirtualMemory(GetCurrentProcess(), 1, &entry, 0) == 0) {
      return std::error_code(static_cast<int>(GetLastError()),
                             std::system_category());
    }
#else
    (void)region;
#endif
    return {};
  }

  std::error_code advise_dontneed(std::span<std::byte> region) override {
    (void)region;
    return {};
  }

  // Windows has no file-offset readahead call that beats
  // PrefetchVirtualMemory, so the readahead hint stays on
  // advise_willneed(); see Volume::maybe_advise_readahead.
  std::error_code advise_readahead(uint64_t file_offset,
                                   size_t length) override {
    (void)file_offset;
    (void)length;
    return std::make_error_code(std::errc::not_supported);
  }

  bool supports_advise_readahead() const noexcept override { return false; }

  uint64_t file_size() const override { return _file_size; }

  const std::byte *persistent_base() const override { return _persistent_base; }
  intptr_t native_fd() const override {
    return reinterpret_cast<intptr_t>(_file_handle);
  }

 private:
  HANDLE _file_handle = INVALID_HANDLE_VALUE;
  uint64_t _file_size = 0;
  OpenMode _mode = OpenMode::ReadOnly;
  std::byte *_persistent_base = nullptr;
  size_t _persistent_size = 0;
  HANDLE _persistent_mapping = INVALID_HANDLE_VALUE;
};

#endif  // CYCLONE_USE_WIN32_MMAP

std::unique_ptr<MappedFile> MappedFile::create() {
#ifdef CYCLONE_USE_WIN32_MMAP
  return std::make_unique<Win32MappedFile>();
#else
  return std::make_unique<PosixMappedFile>();
#endif
}

}  // namespace cyclone
