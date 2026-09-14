// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <system_error>

namespace cyclone {

class MappedFile {
 public:
  enum class MapMode : std::uint8_t { ReadOnly, ReadWrite };

  enum class SyncMode : std::uint8_t { Async, Sync };

  enum class OpenMode : std::uint8_t { ReadOnly, ReadWrite, Create };

  virtual ~MappedFile() = default;

  virtual std::expected<void, std::error_code> open(const std::string& path,
                                                    OpenMode mode) = 0;
  virtual void close() = 0;
  [[nodiscard]] virtual bool is_open() const = 0;

  virtual std::expected<void, std::error_code> map_whole_file(MapMode mode) = 0;

  virtual std::expected<std::span<std::byte>, std::error_code> map_region(
      uint64_t offset, size_t length, MapMode mode) = 0;

  virtual std::error_code unmap_region(std::span<std::byte> region) = 0;

  virtual std::error_code sync(std::span<std::byte> region, SyncMode mode) = 0;

  virtual std::error_code advise_sequential(std::span<std::byte> region) = 0;
  virtual std::error_code advise_random(std::span<std::byte> region) = 0;
  virtual std::error_code advise_willneed(std::span<std::byte> region) = 0;
  virtual std::error_code advise_dontneed(std::span<std::byte> region) = 0;

  [[nodiscard]] virtual uint64_t file_size() const = 0;

  // Return the base address of the persistent whole-file mapping, or nullptr
  // if map_whole_file() was not called or failed.
  [[nodiscard]] virtual const std::byte* persistent_base() const = 0;

  // Return the native file descriptor (POSIX: int cast to intptr_t,
  // Windows: HANDLE cast to intptr_t).  Returns -1 if not open.
  [[nodiscard]] virtual intptr_t native_fd() const = 0;

  static std::unique_ptr<MappedFile> create();
};

}  // namespace cyclone
