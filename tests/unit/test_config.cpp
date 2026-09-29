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

namespace {

// Every CacheConfig field except fill_large_document_tail and write_behind,
// which the KV-tier preset changes.  A field added to CacheConfig (or to the
// nested OptimizationConfig / MultiProcessConfig) must be added here, so the
// preset test keeps covering the whole struct.
void require_same_except_preset_fields(const CacheConfig &a,
                                       const CacheConfig &b) {
  REQUIRE(a.ram_cache_size == b.ram_cache_size);
  REQUIRE(a.ram_cache_type == b.ram_cache_type);
  REQUIRE(a.max_mapped_size == b.max_mapped_size);
  REQUIRE(a.directory_entry_overhead == b.directory_entry_overhead);
  REQUIRE(a.num_segments == b.num_segments);
  REQUIRE(a.max_object_size == b.max_object_size);
  REQUIRE(a.target_frag_size == b.target_frag_size);
  REQUIRE(a.avg_object_size == b.avg_object_size);
  REQUIRE(a.bucket_multiplier == b.bucket_multiplier);
  REQUIRE(a.gc_interval == b.gc_interval);
  REQUIRE(a.gc_evacuate_threshold == b.gc_evacuate_threshold);
  REQUIRE(a.enable_checksum == b.enable_checksum);
  REQUIRE(a.verify_checksum_on_read == b.verify_checksum_on_read);
  REQUIRE(a.enable_compression == b.enable_compression);
  REQUIRE(a.io_queue_depth == b.io_queue_depth);
  REQUIRE(a.readahead_min_bytes == b.readahead_min_bytes);
  REQUIRE(a.cold_readahead_min_bytes == b.cold_readahead_min_bytes);
  REQUIRE(a.sequential_readahead_bytes == b.sequential_readahead_bytes);
  REQUIRE(a.hit_flush_interval == b.hit_flush_interval);
  REQUIRE(a.hit_flush_threshold == b.hit_flush_threshold);
  REQUIRE(a.enable_hit_tracking == b.enable_hit_tracking);
  REQUIRE(a.small_tier_percent == b.small_tier_percent);
  REQUIRE(a.gc_superseded_on_start == b.gc_superseded_on_start);
  REQUIRE(a.unlink_superseded_alternates == b.unlink_superseded_alternates);
  REQUIRE(a.wrap_retention == b.wrap_retention);
  REQUIRE(a.cross_process_ram_coherence == b.cross_process_ram_coherence);
  REQUIRE(a.directory_sync_interval == b.directory_sync_interval);
  REQUIRE(a.read_lease_duration == b.read_lease_duration);
  REQUIRE(a.lease_wrap_ceiling == b.lease_wrap_ceiling);
  REQUIRE(a.alternate_selector == b.alternate_selector);

  const OptimizationConfig &oa = a.optimization_config;
  const OptimizationConfig &ob = b.optimization_config;
  REQUIRE(oa.min_threads == ob.min_threads);
  REQUIRE(oa.max_threads == ob.max_threads);
  REQUIRE(oa.scale_up_threshold == ob.scale_up_threshold);
  REQUIRE(oa.scale_down_threshold == ob.scale_down_threshold);
  REQUIRE(oa.scale_check_interval == ob.scale_check_interval);
  REQUIRE(oa.max_cpu_usage == ob.max_cpu_usage);
  REQUIRE(oa.max_io_bandwidth_fraction == ob.max_io_bandwidth_fraction);
  REQUIRE(oa.max_queue_size == ob.max_queue_size);
  REQUIRE(oa.max_memory_bytes == ob.max_memory_bytes);
  REQUIRE(oa.load_high_watermark == ob.load_high_watermark);
  REQUIRE(oa.load_low_watermark == ob.load_low_watermark);
  REQUIRE(oa.load_shedding_cooldown == ob.load_shedding_cooldown);
  REQUIRE(oa.baseline_ops_per_sec == ob.baseline_ops_per_sec);
  REQUIRE(oa.baseline_bytes_per_sec == ob.baseline_bytes_per_sec);
  REQUIRE(oa.prioritize_by_hit_count == ob.prioritize_by_hit_count);
  REQUIRE(oa.min_hits_before_optimize == ob.min_hits_before_optimize);
  REQUIRE(oa.enabled == ob.enabled);

  const MultiProcessConfig &ma = a.multi_process_config;
  const MultiProcessConfig &mb = b.multi_process_config;
  REQUIRE(ma.enabled == mb.enabled);
  REQUIRE(ma.process_index == mb.process_index);
  REQUIRE(ma.total_processes == mb.total_processes);
  REQUIRE(ma.max_read_retries == mb.max_read_retries);
}

}  // namespace

TEST_CASE("CacheConfig::for_kv_tier sets exactly the documented fields",
          "[config][kv_preset]") {
  const CacheConfig defaults;
  const CacheConfig kv = CacheConfig::for_kv_tier();

  // The two fields the preset documents, on...
  REQUIRE(kv.fill_large_document_tail);
  REQUIRE(kv.write_behind);
  // ...and off in the library defaults, which the preset does not change.
  REQUIRE_FALSE(defaults.fill_large_document_tail);
  REQUIRE_FALSE(defaults.write_behind);

  // Everything else is the library default, wrap retention and the
  // readahead settings included.
  require_same_except_preset_fields(kv, defaults);
  REQUIRE(kv.wrap_retention == detail::default_wrap_retention());
}

TEST_CASE("CacheConfig::for_kv_tier composes with the fluent setters",
          "[config][kv_preset]") {
  CacheConfig kv = CacheConfig::for_kv_tier();
  kv.set_ram_cache_size(0).set_write_behind(false);
  REQUIRE(kv.ram_cache_size == 0);
  REQUIRE_FALSE(kv.write_behind);
  REQUIRE(kv.fill_large_document_tail);
}
