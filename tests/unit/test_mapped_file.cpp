// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "../../src/io/mapped_file.hpp"

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
