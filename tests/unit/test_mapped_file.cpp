// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

#include "../../src/io/mapped_file.hpp"

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace cyclone;

namespace {

std::filesystem::path create_temp_file(size_t size) {
  auto path = std::filesystem::temp_directory_path() / "cyclone_test_XXXXXX";
  std::string path_str = path.string();

  FILE *f = std::fopen(path_str.c_str(), "wb");
  if (f != nullptr) {
    std::vector<char> data(size, 'X');
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
  }

  return path_str;
}

void remove_temp_file(const std::filesystem::path &path) {
  std::filesystem::remove(path);
}

}  // namespace

TEST_CASE("MappedFile create", "[mapped_file]") {
  auto mf = MappedFile::create();
  REQUIRE(mf != nullptr);
  REQUIRE_FALSE(mf->is_open());
}

TEST_CASE("MappedFile open and close", "[mapped_file]") {
  auto path = create_temp_file(4096);
  auto mf = MappedFile::create();

  auto result = mf->open(path.string(), MappedFile::OpenMode::ReadOnly);
  REQUIRE(result.has_value());
  REQUIRE(mf->is_open());
  REQUIRE(mf->file_size() == 4096);

  mf->close();
  REQUIRE_FALSE(mf->is_open());

  remove_temp_file(path);
}

TEST_CASE("MappedFile map region", "[mapped_file]") {
  auto path = create_temp_file(4096);
  auto mf = MappedFile::create();

  mf->open(path.string(), MappedFile::OpenMode::ReadOnly);

  auto region = mf->map_region(0, 1024, MappedFile::MapMode::ReadOnly);
  REQUIRE(region.has_value());
  REQUIRE(region->size() == 1024);
  REQUIRE((*region)[0] == std::byte{'X'});

  auto unmap_result = mf->unmap_region(*region);
  REQUIRE_FALSE(unmap_result);

  mf->close();
  remove_temp_file(path);
}

TEST_CASE("MappedFile read-write mapping", "[mapped_file]") {
  auto path = create_temp_file(4096);
  auto mf = MappedFile::create();

  mf->open(path.string(), MappedFile::OpenMode::ReadWrite);

  auto region = mf->map_region(0, 1024, MappedFile::MapMode::ReadWrite);
  REQUIRE(region.has_value());

  (*region)[0] = std::byte{'Y'};

  auto sync_result = mf->sync(*region, MappedFile::SyncMode::Sync);
  REQUIRE_FALSE(sync_result);

  mf->unmap_region(*region);
  mf->close();

  mf->open(path.string(), MappedFile::OpenMode::ReadOnly);
  auto verify = mf->map_region(0, 1024, MappedFile::MapMode::ReadOnly);
  REQUIRE((*verify)[0] == std::byte{'Y'});

  mf->unmap_region(*verify);
  mf->close();
  remove_temp_file(path);
}

TEST_CASE("MappedFile advise functions", "[mapped_file]") {
  auto path = create_temp_file(4096);
  auto mf = MappedFile::create();

  mf->open(path.string(), MappedFile::OpenMode::ReadOnly);
  auto region = mf->map_region(0, 4096, MappedFile::MapMode::ReadOnly);
  REQUIRE(region.has_value());

  REQUIRE_FALSE(mf->advise_sequential(*region));
  REQUIRE_FALSE(mf->advise_random(*region));
  REQUIRE_FALSE(mf->advise_willneed(*region));
  REQUIRE_FALSE(mf->advise_dontneed(*region));

  mf->unmap_region(*region);
  mf->close();
  remove_temp_file(path);
}

// =============================================================================
// Phase 6B: MappedFile Error and Advise Tests
// =============================================================================

TEST_CASE("MappedFile open non-existent file", "[mapped_file][error]") {
  auto mf = MappedFile::create();
  REQUIRE(mf != nullptr);

  // Try to open a path that definitely does not exist
  std::string nonexistent_path =
      (std::filesystem::temp_directory_path() /
       "cyclone_nonexistent_file_that_does_not_exist_12345.dat")
          .string();

  auto result = mf->open(nonexistent_path, MappedFile::OpenMode::ReadOnly);
  REQUIRE_FALSE(result.has_value());
  // The error should be set (exact code varies by platform)
  REQUIRE(result.error());
  REQUIRE_FALSE(mf->is_open());
}

TEST_CASE("MappedFile advise methods", "[mapped_file]") {
  auto path = create_temp_file(8192);
  auto mf = MappedFile::create();

  auto open_result = mf->open(path.string(), MappedFile::OpenMode::ReadOnly);
  REQUIRE(open_result.has_value());

  auto region = mf->map_region(0, 8192, MappedFile::MapMode::ReadOnly);
  REQUIRE(region.has_value());

  // Call each advise method and verify none crash.
  // On POSIX they call madvise; on Windows they are no-ops.
  // The return value may be a zero error_code (success) or a non-zero error
  // code on platforms that don't support the hint. We just check they don't
  // crash.
  auto seq_err = mf->advise_sequential(*region);
  (void)seq_err;  // May succeed or fail depending on platform

  auto rand_err = mf->advise_random(*region);
  (void)rand_err;

  auto willneed_err = mf->advise_willneed(*region);
  (void)willneed_err;

  auto dontneed_err = mf->advise_dontneed(*region);
  (void)dontneed_err;

  // Verify the region data is still accessible after advise calls
  REQUIRE((*region)[0] == std::byte{'X'});

  mf->unmap_region(*region);
  mf->close();
  remove_temp_file(path);
}

TEST_CASE("MappedFile advise_readahead addresses the file, not the mapping",
          "[mapped_file]") {
  // advise_readahead() is the file-offset readahead hint (Darwin's
  // fcntl(F_RDADVISE)), as opposed to advise_willneed()'s address-range
  // advice.  Two properties are contractual for Volume's read path: it
  // needs no mapping at all, and supports_advise_readahead() agrees with
  // what the call actually does -- that predicate, and not an error code,
  // is what makes the caller fall back to advise_willneed().
  auto path = create_temp_file(8192);
  auto mf = MappedFile::create();

  auto open_result = mf->open(path.string(), MappedFile::OpenMode::ReadOnly);
  REQUIRE(open_result.has_value());

  const bool supported = mf->supports_advise_readahead();
  const auto ec = mf->advise_readahead(0, 8192);
  if (supported) {
    REQUIRE_FALSE(ec);
  } else {
    REQUIRE(ec == std::errc::not_supported);
  }

  // A zero-length hint is a no-op, never a failure, and a hint that runs
  // off the end of the file is still only a hint.
  const auto zero_ec = mf->advise_readahead(0, 0);
  REQUIRE((!zero_ec || zero_ec == std::errc::not_supported));
  (void)mf->advise_readahead(4096, 1 << 20);

  // The mapping is unaffected by any of it.
  auto region = mf->map_region(0, 8192, MappedFile::MapMode::ReadOnly);
  REQUIRE(region.has_value());
  REQUIRE((*region)[0] == std::byte{'X'});
  mf->unmap_region(*region);

  mf->close();
  // On a closed file it reports an error rather than touching a stale
  // descriptor; either way it must not be treated as success by a caller.
  const auto closed_ec = mf->advise_readahead(0, 4096);
  if (supported) {
    REQUIRE(closed_ec == std::errc::bad_file_descriptor);
  }
  remove_temp_file(path);
}

#ifndef _WIN32
TEST_CASE("MappedFile advise_willneed accepts a mid-page region over 4 MiB",
          "[mapped_file]") {
  // Pins the POSIX advise_willneed() contract the readahead path relies on:
  // a document starts at an arbitrary file offset, so the region handed in
  // starts mid-page.  madvise() rejects an unaligned start with EINVAL, so
  // the implementation must round down to the page boundary; and the region
  // is advised in several calls -- 64 KiB chunks over its first 4 MiB, then
  // 512 KiB chunks -- each starting on the previous chunk's end.  Both used
  // to be silent failures -- the hint never ran -- so this asserts SUCCESS,
  // not merely "does not crash".
  constexpr size_t kFileBytes = size_t{6} << 20;  // 6 MiB
  auto path = create_temp_file(kFileBytes);
#if defined(__linux__)
  // The file was just written, so its pages are in the page cache, and
  // advise_willneed() returns early on a resident range.  Write them back
  // and drop them so the chunked madvise() path is what runs here.
  {
    const int fd = ::open(path.string().c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    (void)::fsync(fd);
    (void)::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    ::close(fd);
  }
#endif
  auto mf = MappedFile::create();

  auto open_result = mf->open(path.string(), MappedFile::OpenMode::ReadOnly);
  REQUIRE(open_result.has_value());

  auto mapped = mf->map_region(0, kFileBytes, MappedFile::MapMode::ReadOnly);
  REQUIRE(mapped.has_value());

  // Starts 100 bytes into the first page and runs 4700 KiB: unaligned
  // start, every 64 KiB head chunk, then one full and one partial 512 KiB
  // tail chunk, ends mid-page, and stays inside the mapping.
  constexpr size_t kStart = 100;
  constexpr size_t kLength = size_t{4700} * 1024;
  static_assert(kStart + kLength <= kFileBytes);
  auto region = mapped->subspan(kStart, kLength);

  const auto ec = mf->advise_willneed(region);
  INFO("advise_willneed error: " << ec.message());
  REQUIRE_FALSE(ec);

  // The advice is a pure hint: contents are untouched.
  REQUIRE(region[0] == std::byte{'X'});
  REQUIRE(region[kLength - 1] == std::byte{'X'});

  mf->unmap_region(*mapped);
  mf->close();
  remove_temp_file(path);
}
#endif
