// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

//
// Cyclone Cache - Basic Usage Example
//
// This example demonstrates the core functionality of Cyclone Cache:
// - Creating and configuring a cache
// - Writing content to the cache
// - Reading content from the cache
// - Using the synchronous API
//

#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/key.hpp"

using namespace cyclone;

int main() {
  std::cout << "Cyclone Cache - Basic Usage Example\n";
  std::cout << "====================================\n\n";

  // Configure the cache
  CacheConfig config;
  config.set_ram_cache_size(64_MB);
  config.set_ram_cache_type(RamCacheType::CLFUS);

  // Create the cache
  auto cache_result = Cache::create(config);
  if (!cache_result) {
    std::cerr << "Failed to create cache\n";
    return 1;
  }

  auto &cache = *cache_result.value();

  // Create a temporary directory for the cache volume
  auto temp_path = std::filesystem::temp_directory_path() / "cyclone_example";
  std::filesystem::create_directories(temp_path);

  std::string volume_path = (temp_path / "cache.dat").string();

  // Add a volume (100MB)
  auto add_result = cache.add_volume(volume_path, 100_MB);
  if (!add_result) {
    std::cerr << "Failed to add volume\n";
    return 1;
  }

  // Start the cache
  auto start_result = cache.start();
  if (!start_result) {
    std::cerr << "Failed to start cache\n";
    return 1;
  }

  std::cout << "Cache started successfully\n";
  std::cout << "  Volumes: " << cache.volume_count() << "\n";
  std::cout << "  Total capacity: " << cache.total_capacity() / (1024 * 1024)
            << " MB\n\n";

  // Create a cache key
  CacheKey key = CacheKey::from_url("http://example.com/resource.html");
  std::cout << "Cache key: " << key.to_hex().substr(0, 16) << "...\n\n";

  // Write some content to the cache
  std::string content = "Hello, Cyclone Cache! This is cached content.";
  std::string header = "Content-Type: text/plain";

  std::cout << "Writing to cache:\n";
  std::cout << "  Header: " << header << "\n";
  std::cout << "  Content: " << content << "\n\n";

  auto write_result = cache.write_sync(key, content.size());
  if (!write_result) {
    std::cerr << "Failed to open write handle\n";
    return 1;
  }

  auto &write_handle = write_result.value();

  // Set header and content
  std::vector<std::byte> header_bytes(header.size());
  std::memcpy(header_bytes.data(), header.data(), header.size());
  write_handle.set_header(header_bytes);

  std::vector<std::byte> content_bytes(content.size());
  std::memcpy(content_bytes.data(), content.data(), content.size());

  auto write_task = write_handle.write(content_bytes);
  write_task.sync_wait();

  auto close_task = write_handle.close();
  close_task.sync_wait();

  std::cout << "Content written successfully\n\n";

  // Read the content back
  std::cout << "Reading from cache:\n";

  auto read_result = cache.read_sync(key);
  if (!read_result) {
    std::cerr << "Failed to read from cache (entry may not be written yet)\n";
    // Note: In a real application, you would handle this case appropriately
  } else {
    auto &read_handle = read_result.value();

    if (read_handle.is_ram_cache_hit()) {
      std::cout << "  (RAM cache hit!)\n";
    }

    auto read_header = read_handle.header();
    if (!read_header.empty()) {
      std::string h(reinterpret_cast<const char *>(read_header.data()),
                    read_header.size());
      std::cout << "  Header: " << h << "\n";
    }

    std::cout << "  Content length: " << read_handle.content_length()
              << " bytes\n";

    read_handle.close();
  }

  // Show cache statistics
  std::cout << "\nCache Statistics:\n";
  auto stats = cache.stats();
  std::cout << "  RAM cache hits: " << stats.ram_cache_hits << "\n";
  std::cout << "  RAM cache misses: " << stats.ram_cache_misses << "\n";
  std::cout << "  Disk cache hits: " << stats.disk_cache_hits << "\n";
  std::cout << "  Current entries: " << stats.current_entries << "\n";
  std::cout << "  Bytes used: " << stats.current_bytes << "\n";

  // Stop the cache
  cache.stop();
  std::cout << "\nCache stopped\n";

  // Cleanup
  std::filesystem::remove_all(temp_path);

  return 0;
}
