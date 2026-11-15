// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// What Volume::open() uses at the volume path: the volume must be a plain
// file owned by the configured user.  POSIX only: the checks are compiled out
// on Windows.

#include <catch2/catch_test_macros.hpp>

#ifndef _WIN32

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/volume.hpp"
#include "cyclone/config.hpp"
#include "cyclone/error.hpp"
#include "support/temp_cache.hpp"

using namespace cyclone;
namespace fs = std::filesystem;

namespace {

constexpr size_t kVolumeSize = size_t{8} << 20;

VolumeConfig volume_config(const fs::path& path) {
  VolumeConfig config;
  config.path = path.string();
  config.size = kVolumeSize;
  return config;
}

// A small file with known contents that must come through untouched.
void write_marker(const fs::path& path) {
  std::ofstream out(path, std::ios::binary);
  out << "unrelated file contents";
}

std::string read_all(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE("Volume open creates and reopens a regular file",
          "[volume][volume_file]") {
  TempCacheDir dir("volfile_ok");
  const fs::path path = dir.dir() / "cyclone.dat";
  {
    Volume volume(volume_config(path));
    REQUIRE(volume.open().has_value());
    volume.close();
  }
  REQUIRE(fs::is_regular_file(path));
  REQUIRE(fs::file_size(path) == kVolumeSize);
  {
    Volume volume(volume_config(path));
    REQUIRE(volume.open().has_value());
    volume.close();
  }
}

TEST_CASE("Volume open refuses a path that is not a plain file",
          "[volume][volume_file]") {
  TempCacheDir dir("volfile_symlink");
  const fs::path target = dir.dir() / "elsewhere.txt";
  const fs::path path = dir.dir() / "cyclone.dat";
  write_marker(target);
  fs::create_symlink(target, path);

  Volume volume(volume_config(path));
  auto opened = volume.open();
  REQUIRE_FALSE(opened.has_value());
  REQUIRE(opened.error() == CacheError::VolumeFileRefused);
  // The link's target is neither resized nor written.
  REQUIRE(read_all(target) == "unrelated file contents");
}

TEST_CASE("Volume open refuses a FIFO at the volume path",
          "[volume][volume_file]") {
  TempCacheDir dir("volfile_fifo");
  const fs::path path = dir.dir() / "cyclone.dat";
  REQUIRE(::mkfifo(path.c_str(), 0600) == 0);

  Volume volume(volume_config(path));
  auto opened = volume.open();
  REQUIRE_FALSE(opened.has_value());
  REQUIRE(opened.error() == CacheError::VolumeFileRefused);
}

TEST_CASE("Volume open refuses a file with a second link",
          "[volume][volume_file]") {
  TempCacheDir dir("volfile_hardlink");
  const fs::path other = dir.dir() / "elsewhere.txt";
  const fs::path path = dir.dir() / "cyclone.dat";
  write_marker(other);
  fs::create_hard_link(other, path);

  Volume volume(volume_config(path));
  auto opened = volume.open();
  REQUIRE_FALSE(opened.has_value());
  REQUIRE(opened.error() == CacheError::VolumeFileRefused);
  REQUIRE(read_all(other) == "unrelated file contents");
}

TEST_CASE("Volume open accepts a file owned by the configured owner",
          "[volume][volume_file]") {
  TempCacheDir dir("volfile_owner_ok");
  const fs::path path = dir.dir() / "cyclone.dat";
  VolumeConfig config = volume_config(path);
  config.owner = VolumeConfig::Owner{static_cast<uint32_t>(::geteuid()),
                                     static_cast<uint32_t>(::getegid())};
  for (int pass = 0; pass < 2; ++pass) {  // create, then reopen
    CAPTURE(pass);
    Volume volume(config);
    REQUIRE(volume.open().has_value());
    volume.close();
  }
}

TEST_CASE("Volume open refuses a file owned by another user than the owner",
          "[volume][volume_file]") {
  if (::geteuid() == 0) {
    SKIP("as root the file is handed over instead; see the next case");
  }
  TempCacheDir dir("volfile_owner_other");
  const fs::path path = dir.dir() / "cyclone.dat";
  VolumeConfig config = volume_config(path);
  config.owner = VolumeConfig::Owner{static_cast<uint32_t>(::geteuid()) + 1,
                                     static_cast<uint32_t>(::getegid())};
  Volume volume(config);
  auto opened = volume.open();
  REQUIRE_FALSE(opened.has_value());
  REQUIRE(opened.error() == CacheError::VolumeFileRefused);
}

TEST_CASE("Volume open as root gives a root-owned volume to the owner",
          "[volume][volume_file]") {
  if (::geteuid() != 0) {
    SKIP("needs root");
  }
  TempCacheDir dir("volfile_owner_handover");
  const fs::path path = dir.dir() / "cyclone.dat";
  constexpr uint32_t kWorker = 65534;  // conventionally "nobody"
  VolumeConfig config = volume_config(path);
  config.owner = VolumeConfig::Owner{kWorker, kWorker};
  {
    Volume volume(config);
    REQUIRE(volume.open().has_value());
    volume.close();
  }
  struct stat st{};
  REQUIRE(::stat(path.c_str(), &st) == 0);
  REQUIRE(st.st_uid == kWorker);
  REQUIRE(st.st_gid == kWorker);

  // An existing root-owned volume (an earlier root run's) is given too.
  REQUIRE(::chown(path.c_str(), 0, 0) == 0);
  {
    Volume volume(config);
    REQUIRE(volume.open().has_value());
    volume.close();
  }
  REQUIRE(::stat(path.c_str(), &st) == 0);
  REQUIRE(st.st_uid == kWorker);

  // A file some third user owns is not used, even as root.
  REQUIRE(::chown(path.c_str(), kWorker - 1, kWorker - 1) == 0);
  {
    Volume volume(config);
    auto opened = volume.open();
    REQUIRE_FALSE(opened.has_value());
    REQUIRE(opened.error() == CacheError::VolumeFileRefused);
  }

  // Nor is a root-owned file that is not a volume, and it keeps its owner.
  const fs::path other = dir.dir() / "other.dat";
  write_marker(other);
  VolumeConfig other_config = config;
  other_config.path = other.string();
  {
    Volume volume(other_config);
    auto opened = volume.open();
    REQUIRE_FALSE(opened.has_value());
    REQUIRE(opened.error() == CacheError::VolumeFileRefused);
  }
  REQUIRE(::stat(other.c_str(), &st) == 0);
  REQUIRE(st.st_uid == 0);
  REQUIRE(read_all(other) == "unrelated file contents");
}

#endif  // !_WIN32
