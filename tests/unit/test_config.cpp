// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <thread>

#include "cyclone/config.hpp"

using namespace cyclone;

TEST_CASE("CacheConfig fluent builder", "[config]") {
  CacheConfig cfg;

  // Chain multiple set_*() calls and verify all values
  cfg.set_ram_cache_size(128_MB)
      .set_ram_cache_type(RamCacheType::LRU)
      .set_max_mapped_size(512_MB)
      .set_num_segments(8)
      .set_enable_checksum(false)
      .set_multi_process(2, 4);

  REQUIRE(cfg.ram_cache_size == 128_MB);
  REQUIRE(cfg.ram_cache_type == RamCacheType::LRU);
  REQUIRE(cfg.max_mapped_size == 512_MB);
  REQUIRE(cfg.num_segments == 8);
  REQUIRE(cfg.enable_checksum == false);

  // set_multi_process should enable multi-process and set index/total
  REQUIRE(cfg.multi_process_config.enabled == true);
  REQUIRE(cfg.multi_process_config.process_index == 2);
  REQUIRE(cfg.multi_process_config.total_processes == 4);

  // Test set_multi_process_config with a standalone MultiProcessConfig
  MultiProcessConfig mp;
  mp.set_enabled(true)
      .set_process_index(1)
      .set_total_processes(3)
      .set_max_read_retries(5);

  cfg.set_multi_process_config(mp);

  REQUIRE(cfg.multi_process_config.enabled == true);
  REQUIRE(cfg.multi_process_config.process_index == 1);
  REQUIRE(cfg.multi_process_config.total_processes == 3);
  REQUIRE(cfg.multi_process_config.max_read_retries == 5);

  // Test set_alternate_selector with nullptr (just exercises the setter)
  cfg.set_alternate_selector(nullptr);
  REQUIRE(cfg.alternate_selector == nullptr);
}

TEST_CASE("OptimizationConfig effective_max_threads", "[config]") {
  SECTION("max_threads=0 gives auto value") {
    OptimizationConfig opt;
    REQUIRE(opt.max_threads == 0);

    size_t effective = opt.effective_max_threads();
    REQUIRE(effective >= 1);

    size_t hw = std::thread::hardware_concurrency();
    if (hw > 2) {
      REQUIRE(effective == hw / 2);
    } else {
      REQUIRE(effective == 1);
    }
  }

  SECTION("explicit max_threads is returned as-is") {
    OptimizationConfig opt;
    opt.set_max_threads(4);
    REQUIRE(opt.effective_max_threads() == 4);
  }

  SECTION("builder methods set values correctly") {
    OptimizationConfig opt;

    opt.set_enabled(false)
        .set_min_threads(2)
        .set_max_threads(8)
        .set_scale_up_threshold(20)
        .set_scale_down_threshold(5)
        .set_scale_check_interval(std::chrono::milliseconds{2000})
        .set_max_cpu_usage(0.75)
        .set_max_io_bandwidth_fraction(0.5)
        .set_max_queue_size(5000)
        .set_max_memory_bytes(512_MB)
        .set_prioritize_by_hit_count(false)
        .set_min_hits_before_optimize(10)
        .set_load_shedding_cooldown(std::chrono::milliseconds{3000})
        .set_baseline_ops_per_sec(200000.0)
        .set_baseline_bytes_per_sec(1_GB);

    REQUIRE(opt.enabled == false);
    REQUIRE(opt.min_threads == 2);
    REQUIRE(opt.max_threads == 8);
    REQUIRE(opt.scale_up_threshold == 20);
    REQUIRE(opt.scale_down_threshold == 5);
    REQUIRE(opt.scale_check_interval == std::chrono::milliseconds{2000});
    REQUIRE(opt.max_cpu_usage == 0.75);
    REQUIRE(opt.max_io_bandwidth_fraction == 0.5);
    REQUIRE(opt.max_queue_size == 5000);
    REQUIRE(opt.max_memory_bytes == 512_MB);
    REQUIRE(opt.prioritize_by_hit_count == false);
    REQUIRE(opt.min_hits_before_optimize == 10);
    REQUIRE(opt.load_shedding_cooldown == std::chrono::milliseconds{3000});
    REQUIRE(opt.baseline_ops_per_sec == 200000.0);
    REQUIRE(opt.baseline_bytes_per_sec == static_cast<double>(1_GB));
    REQUIRE(opt.effective_max_threads() == 8);
  }
}

TEST_CASE("OptimizationConfig watermark invariant", "[config]") {
  SECTION("set_load_high_watermark auto-adjusts low if needed") {
    OptimizationConfig opt;
    // Default: high=0.8, low=0.5. Setting high to 0.4 should adjust low
    // because low (0.5) >= new high (0.4).
    opt.set_load_high_watermark(0.4);
    REQUIRE(opt.load_high_watermark == 0.4);
    REQUIRE(opt.load_low_watermark < opt.load_high_watermark);
    // The adjustment formula is: low = high * 0.6
    REQUIRE(opt.load_low_watermark == Catch::Approx(0.4 * 0.6));
  }

  SECTION("set_load_high_watermark does not adjust low when already valid") {
    OptimizationConfig opt;
    opt.load_low_watermark = 0.3;
    opt.load_high_watermark = 0.5;

    // Setting high to 0.8 should not change low since 0.3 < 0.8
    opt.set_load_high_watermark(0.8);
    REQUIRE(opt.load_high_watermark == 0.8);
    REQUIRE(opt.load_low_watermark == 0.3);
  }

  SECTION("set_load_low_watermark auto-adjusts high if needed") {
    OptimizationConfig opt;
    // Default: high=0.8, low=0.5. Setting low to 0.9 should adjust high
    // because low (0.9) >= high (0.8).
    opt.set_load_low_watermark(0.9);
    REQUIRE(opt.load_low_watermark == 0.9);
    REQUIRE(opt.load_high_watermark > opt.load_low_watermark);
    // The adjustment formula is: high = low / 0.6
    REQUIRE(opt.load_high_watermark == Catch::Approx(0.9 / 0.6));
  }

  SECTION("set_load_low_watermark does not adjust high when already valid") {
    OptimizationConfig opt;
    opt.load_low_watermark = 0.3;
    opt.load_high_watermark = 0.9;

    // Setting low to 0.5 should not change high since 0.5 < 0.9
    opt.set_load_low_watermark(0.5);
    REQUIRE(opt.load_low_watermark == 0.5);
    REQUIRE(opt.load_high_watermark == 0.9);
  }

  SECTION("setting equal values triggers adjustment") {
    OptimizationConfig opt;
    opt.load_high_watermark = 0.6;

    // Setting low = high (0.6) should trigger the adjustment
    opt.set_load_low_watermark(0.6);
    REQUIRE(opt.load_low_watermark == 0.6);
    REQUIRE(opt.load_high_watermark > 0.6);
    REQUIRE(opt.load_high_watermark == Catch::Approx(0.6 / 0.6));
  }
}

TEST_CASE("MultiProcessConfig validation", "[config]") {
  SECTION("disabled config is always valid") {
    MultiProcessConfig mp;
    REQUIRE(mp.enabled == false);
    REQUIRE(mp.is_valid() == true);

    // Even with nonsensical values, disabled is valid
    mp.process_index = 100;
    mp.total_processes = 0;
    REQUIRE(mp.is_valid() == true);
  }

  SECTION("enabled with total_processes=0 is invalid") {
    MultiProcessConfig mp;
    mp.set_enabled(true).set_total_processes(0).set_process_index(0);
    REQUIRE(mp.is_valid() == false);
  }

  SECTION("enabled with process_index >= total_processes is invalid") {
    MultiProcessConfig mp;
    mp.set_enabled(true).set_total_processes(4).set_process_index(4);
    REQUIRE(mp.is_valid() == false);

    mp.set_process_index(5);
    REQUIRE(mp.is_valid() == false);
  }

  SECTION("enabled with process_index < total_processes is valid") {
    MultiProcessConfig mp;
    mp.set_enabled(true).set_total_processes(4).set_process_index(0);
    REQUIRE(mp.is_valid() == true);

    mp.set_process_index(3);
    REQUIRE(mp.is_valid() == true);
  }

  SECTION("builder methods set values correctly") {
    MultiProcessConfig mp;
    mp.set_enabled(true)
        .set_process_index(2)
        .set_total_processes(5)
        .set_max_read_retries(3);

    REQUIRE(mp.enabled == true);
    REQUIRE(mp.process_index == 2);
    REQUIRE(mp.total_processes == 5);
    REQUIRE(mp.max_read_retries == 3);
    REQUIRE(mp.is_valid() == true);
  }
}

TEST_CASE("VolumeConfig defaults", "[config]") {
  VolumeConfig vol;

  REQUIRE(vol.path.empty());
  REQUIRE(vol.size == 0);
  REQUIRE(vol.stripe_size == 0);  // 0 = auto (derive stripe count from size)
  REQUIRE(vol.direct_io == false);
  REQUIRE(vol.sync_on_write == false);
  REQUIRE(vol.auto_reset_on_incompatible == true);
  REQUIRE(vol.max_fragments == 0);
  REQUIRE(vol.ram_cache_proportion == 1.0);
}
