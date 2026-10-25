// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "../../src/core/volume.hpp"
#include "../../src/io/mapped_file.hpp"
#include "../../src/io/win_path.hpp"
#include "cyclone/key.hpp"
#include "support/temp_cache.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

TEST_CASE("MappedFile map_region refuses a range past the end of the file",
          "[mapped_file][error]") {
  // Without a persistent mapping, map_region() maps each region on demand.
  // A range that reaches past the end of the file must be refused up front,
  // as the persistent-mapping path does: mapping it would succeed, and the
  // first touch of a page wholly beyond the end of the file would raise a
  // bus error instead of returning an error.
  auto path = create_temp_file(4096);
  auto mf = MappedFile::create();
  REQUIRE(mf->open(path.string(), MappedFile::OpenMode::ReadOnly));
  REQUIRE(mf->persistent_base() == nullptr);

  constexpr auto kMode = MappedFile::MapMode::ReadOnly;
  // Whole file and an exact tail still map.
  auto whole = mf->map_region(0, 4096, kMode);
  REQUIRE(whole.has_value());
  REQUIRE_FALSE(mf->unmap_region(*whole));
  auto tail = mf->map_region(4000, 96, kMode);
  REQUIRE(tail.has_value());
  REQUIRE_FALSE(mf->unmap_region(*tail));

  // Starts inside, ends past the end.
  auto straddle = mf->map_region(4000, 8192, kMode);
  REQUIRE_FALSE(straddle.has_value());
  REQUIRE(straddle.error() == std::errc::invalid_argument);
  // Starts past the end.
  auto beyond = mf->map_region(65536, 16, kMode);
  REQUIRE_FALSE(beyond.has_value());
  REQUIRE(beyond.error() == std::errc::invalid_argument);
  // offset + length would wrap around.
  auto wrap = mf->map_region(16, SIZE_MAX - 8, kMode);
  REQUIRE_FALSE(wrap.has_value());
  REQUIRE(wrap.error() == std::errc::invalid_argument);

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

TEST_CASE("MappedFile range_resident is true only where it can tell",
          "[mapped_file]") {
  // The sequential readahead window skips a hint when range_resident() says
  // the range is cached, so the one thing it must never do is report a range
  // resident that is not: false is always a safe answer.  On Linux a range
  // this process has just read is resident (mincore()); elsewhere the answer
  // is always false.
  constexpr size_t kFileBytes = size_t{256} * 1024;
  auto path = create_temp_file(kFileBytes);
  auto mf = MappedFile::create();
  REQUIRE(mf->open(path.string(), MappedFile::OpenMode::ReadOnly).has_value());
  auto mapped = mf->map_region(0, kFileBytes, MappedFile::MapMode::ReadOnly);
  REQUIRE(mapped.has_value());

  // Touch every byte: the whole range is now in the page cache.
  unsigned sum = 0;
  for (std::byte b : *mapped) {
    sum += static_cast<unsigned>(b);
  }
  REQUIRE(sum != 0);

  // Mid-page start and end, as a document range has.
  const auto region = mapped->subspan(100, kFileBytes - 200);
#if defined(__linux__)
  CHECK(mf->range_resident(region));
  CHECK(mf->range_resident(region.first(1)));
#else
  CHECK_FALSE(mf->range_resident(region));
#endif

  // The unchecked hint advises a resident, mid-page range without error and
  // leaves the contents alone.
  const auto ec = mf->advise_willneed_unchecked(mapped->subspan(100, 70000));
  INFO("advise_willneed_unchecked error: " << ec.message());
  CHECK_FALSE(ec);
  CHECK((*mapped)[100] == std::byte{'X'});

  mf->unmap_region(*mapped);
  mf->close();
  remove_temp_file(path);
}

TEST_CASE(
    "Volume surfaces a failed whole-file mapping and keeps working on the "
    "per-operation fallback",
    "[mapped_file][volume][regression]") {
  struct Reset {
    ~Reset() { MappedFile::set_whole_file_map_failure_for_testing(false); }
  } reset;

  auto open_and_roundtrip = [](bool force_failure) -> uint64_t {
    MappedFile::set_whole_file_map_failure_for_testing(force_failure);
    TempCacheDir tmp;
    VolumeConfig config;
    config.path = tmp.path();
    config.size = static_cast<size_t>(10 * 1024 * 1024);
    Volume volume(config);
    REQUIRE(volume.open().has_value());

    CacheKey key("whole-file-map-fallback");
    std::vector<std::byte> payload(256, std::byte{0x5A});
    auto wh = volume.write_sync(key, payload.size());
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(std::span<const std::byte>(payload)).has_value());
    REQUIRE(wh->close_sync().has_value());
    auto rh = volume.read_sync(key);
    REQUIRE(rh.has_value());
    REQUIRE(rh->content().size() == payload.size());
    return volume.stats().whole_file_map_failed;
  };

  REQUIRE(open_and_roundtrip(false) == 0);
  REQUIRE(open_and_roundtrip(true) == 1);
}

#ifdef _WIN32
TEST_CASE("MappedFile handles allow another open to delete the file",
          "[mapped_file][windows]") {
  for (auto mode :
       {MappedFile::OpenMode::ReadOnly, MappedFile::OpenMode::ReadWrite}) {
    auto path = create_temp_file(4096);
    auto mf = MappedFile::create();
    REQUIRE(mf->open(path.string(), mode).has_value());
    const auto map_mode = mode == MappedFile::OpenMode::ReadOnly
                              ? MappedFile::MapMode::ReadOnly
                              : MappedFile::MapMode::ReadWrite;
    REQUIRE(mf->map_whole_file(map_mode).has_value());
    REQUIRE(mf->persistent_base() != nullptr);
    REQUIRE(static_cast<unsigned char>(mf->persistent_base()[0]) == 'X');

    // Deleting needs every open handle to allow FILE_SHARE_DELETE.
    std::error_code ec;
    std::filesystem::remove(path, ec);
    REQUIRE_FALSE(ec);

    // The mapping stays readable after the name is gone.
    REQUIRE(static_cast<unsigned char>(mf->persistent_base()[0]) == 'X');

    mf->close();
    std::filesystem::remove(path, ec);
  }
}

TEST_CASE("A live Volume can be renamed and deleted by path",
          "[mapped_file][volume][windows]") {
  TempCacheDir tmp;
  VolumeConfig config;
  config.path = tmp.path();
  config.size = static_cast<size_t>(10 * 1024 * 1024);
  Volume volume(config);
  REQUIRE(volume.open().has_value());

  CacheKey key("rename-delete-live-volume");
  std::vector<std::byte> payload(256, std::byte{0x5A});
  auto wh = volume.write_sync(key, payload.size());
  REQUIRE(wh.has_value());
  REQUIRE(wh->write_sync(std::span<const std::byte>(payload)).has_value());
  REQUIRE(wh->close_sync().has_value());

  // Both the descriptor and the mapping of the open volume must allow delete
  // sharing, or these path-based calls fail with a sharing violation.
  const std::filesystem::path original(config.path);
  const std::filesystem::path aside = original.string() + ".aside";
  REQUIRE(MoveFileExW(original.wstring().c_str(), aside.wstring().c_str(), 0) !=
          0);
  REQUIRE(DeleteFileW(aside.wstring().c_str()) != 0);

  auto rh = volume.read_sync(key);
  REQUIRE(rh.has_value());
  REQUIRE(rh->content().size() == payload.size());
}

TEST_CASE("Volume files open through wide-character paths",
          "[mapped_file][volume][windows]") {
  namespace fs = std::filesystem;
  // The ASCII-named parent is removed by TempCacheDir even when a REQUIRE
  // fails, and the non-ASCII directory lives only inside it, so no other test
  // that scans the shared temp root can see it. Paths are built from wide
  // literals so the test never goes through the ANSI code page.
  TempCacheDir tmp("wide");
  const fs::path dir = tmp.dir() / fs::path(
                                       L"\u03c4\u03b5\u03c3\u03c4_"
                                       L"\u30ad\u30e3\u30c3\u30b7\u30e5");
  REQUIRE(fs::create_directory(dir));

  // MappedFile: create the file under the non-ASCII name, fill it, reopen it.
  const fs::path mapped = dir / fs::path(L"mapped.dat");
  {
    auto mf = MappedFile::create();
    REQUIRE(mf->open(path_to_utf8(mapped), MappedFile::OpenMode::Create)
                .has_value());
    mf->close();
  }
  REQUIRE(fs::exists(mapped));
  {
    std::ofstream out(mapped, std::ios::binary);
    const std::string data(4096, 'W');
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    REQUIRE(out.good());
  }
  {
    auto mf = MappedFile::create();
    REQUIRE(mf->open(path_to_utf8(mapped), MappedFile::OpenMode::ReadOnly)
                .has_value());
    REQUIRE(mf->file_size() == 4096);
    REQUIRE(mf->map_whole_file(MappedFile::MapMode::ReadOnly).has_value());
    REQUIRE(static_cast<unsigned char>(mf->persistent_base()[0]) == 'W');
    mf->close();
  }

  // Volume: build the cache path with the same helper the cache layer uses,
  // write a key, close, reopen and read it back. The in-memory directory of a
  // single-process volume is rebuilt empty on every open, so the round trip
  // uses the persistent (memory-mapped) directory that survives a reopen.
  MultiProcessConfig mp;
  mp.set_enabled(true).set_process_index(0).set_total_processes(1);
  VolumeConfig config;
  config.size = static_cast<size_t>(64) * 1024 * 1024;
  const std::string base_path = path_to_utf8(dir / fs::path(L"volume.dat"));
  config.path = fingerprint_cache_path(base_path, config.size, 0, true);
  CAPTURE(base_path, config.path);
  REQUIRE(config.path != base_path);
  REQUIRE(utf8_to_path(config.path).parent_path() == dir);
  // The same input must always name the same file.
  REQUIRE(fingerprint_cache_path(base_path, config.size, 0, true) ==
          config.path);

  CacheKey key("wide-path-round-trip");
  const std::vector<std::byte> payload(512, std::byte{0x37});
  {
    Volume volume(config, mp);
    REQUIRE(volume.open().has_value());
    auto wh = volume.write_sync(key, payload.size());
    REQUIRE(wh.has_value());
    REQUIRE(wh->write_sync(std::span<const std::byte>(payload)).has_value());
    REQUIRE(wh->close_sync().has_value());
    auto same = volume.read_sync(key);
    INFO("read-back on the still-open volume succeeded: " << same.has_value());
  }
  const bool exists_after_close = fs::exists(utf8_to_path(config.path));
  std::error_code size_ec;
  const auto size_after_close =
      fs::file_size(utf8_to_path(config.path), size_ec);
  CAPTURE(exists_after_close, size_after_close, size_ec.message());
  REQUIRE(exists_after_close);

  // The unsized resolver finds the fingerprinted file in the same directory.
  REQUIRE(resolve_unsized_cache_path(base_path) == config.path);

  {
    Volume volume(config, mp);
    REQUIRE(volume.open().has_value());
    auto rh = volume.read_sync(key);
    INFO("reopened volume read_sync has_value: " << rh.has_value());
    REQUIRE(rh.has_value());
    REQUIRE(rh->content().size() == payload.size());
  }
}
#endif
