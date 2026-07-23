// Self-contained test suite -- no external framework, so `cmake --build` and
// `make` work on a bare toolchain.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "ftlsim/ftl/ftl.hpp"
#include "ftlsim/metrics/metrics.hpp"
#include "ftlsim/nand/nand.hpp"
#include "ftlsim/workload/generator.hpp"

using namespace ftlsim;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::cout << "  FAIL (line " << line << "): " << what << "\n";
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)
#define CHECK_EQ(a, b)                                                     \
    do {                                                                   \
        const auto lhs__ = (a);                                            \
        const auto rhs__ = (b);                                            \
        ++g_checks;                                                        \
        if (!(lhs__ == rhs__)) {                                           \
            ++g_failures;                                                  \
            std::cout << "  FAIL (line " << __LINE__ << "): " << #a        \
                      << " == " << #b << "  [" << lhs__ << " vs " << rhs__ \
                      << "]\n";                                            \
        }                                                                  \
    } while (false)

struct TestCase {
    std::string name;
    std::function<void()> body;
};

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

void add_test(const std::string& name, std::function<void()> body) {
    registry().push_back({name, std::move(body)});
}

FtlConfig small_config() {
    FtlConfig config;
    config.blocks = 16;
    config.pages_per_block = 8;
    config.overprovisioning = 0.25;
    config.wear_leveling = WearLevelingMode::None;
    return config;
}

// --- NAND layer -----------------------------------------------------------

void test_block_program_invalidate_erase() {
    Block block(4);
    CHECK(block.is_clean());
    CHECK_EQ(block.free_pages(), 4);

    CHECK_EQ(block.append(10, 1), 0);
    CHECK_EQ(block.append(11, 2), 1);
    CHECK_EQ(block.valid_pages(), 2);
    CHECK_EQ(block.free_pages(), 2);
    CHECK(!block.is_clean());

    block.invalidate(0, 3);
    CHECK_EQ(block.valid_pages(), 1);
    CHECK_EQ(block.invalid_pages(), 1);
    // Invalidating twice must not double-count.
    block.invalidate(0, 4);
    CHECK_EQ(block.invalid_pages(), 1);

    CHECK_EQ(block.append(12, 5), 2);
    CHECK_EQ(block.append(13, 6), 3);
    CHECK(block.is_full());
    CHECK_EQ(block.append(14, 7), -1);  // no room left

    block.erase(8);
    CHECK_EQ(block.erase_count(), 1u);
    CHECK_EQ(block.valid_pages(), 0);
    CHECK_EQ(block.invalid_pages(), 0);
    CHECK(block.is_clean());
    CHECK(block.page(0).is_free());
}

void test_nand_addressing() {
    Nand nand(4, 8);
    CHECK_EQ(nand.total_pages(), 32);
    CHECK_EQ(nand.ppn_of(2, 3), 19);
    CHECK_EQ(nand.block_of(19), 2);
    CHECK_EQ(nand.page_of(19), 3);

    const std::int64_t ppn = nand.program(2, 77, 1);
    CHECK_EQ(ppn, 16);
    CHECK_EQ(nand.page_at(ppn).lpn(), 77);
    CHECK(nand.page_at(ppn).is_valid());
    CHECK_EQ(nand.valid_pages(), 1);

    nand.invalidate(ppn, 2);
    CHECK(nand.page_at(ppn).is_invalid());
    CHECK_EQ(nand.valid_pages(), 0);
    CHECK_EQ(nand.state_string(2).substr(0, 2), std::string("if"));
}

// --- Mapping / FTL basics -------------------------------------------------

void test_read_after_write() {
    Ftl ftl(small_config());
    ftl.write(5);
    const std::int64_t ppn = ftl.read(5);
    CHECK(ppn != kInvalidPpn);
    CHECK_EQ(ftl.nand().page_at(ppn).lpn(), 5);
    CHECK(ftl.nand().page_at(ppn).is_valid());

    // A never-written logical page reads as unmapped.
    CHECK_EQ(ftl.read(6), kInvalidPpn);
    CHECK_EQ(ftl.metrics().read_misses, 1u);
}

void test_out_of_place_write_invalidates_old_page() {
    Ftl ftl(small_config());
    ftl.write(3);
    const std::int64_t first = ftl.read(3);
    ftl.write(3);
    const std::int64_t second = ftl.read(3);

    CHECK(first != second);
    CHECK(ftl.nand().page_at(first).is_invalid());
    CHECK(ftl.nand().page_at(second).is_valid());
    // One live copy of the logical page, regardless of how often it is written.
    CHECK_EQ(ftl.nand().valid_pages(), 1);
    CHECK_EQ(ftl.mapping().mapped_pages(), 1);
}

void test_write_out_of_range_rejected() {
    Ftl ftl(small_config());
    bool threw = false;
    try {
        ftl.write(ftl.logical_pages());
    } catch (const std::out_of_range&) {
        threw = true;
    }
    CHECK(threw);
}

void test_overprovisioning_reserves_capacity() {
    FtlConfig config = small_config();
    config.overprovisioning = 0.25;
    Ftl ftl(config);
    // 16 x 8 = 128 physical pages, 25% held back.
    CHECK_EQ(ftl.logical_pages(), 96);
}

// --- Garbage collection ---------------------------------------------------

void test_gc_reclaims_space_and_preserves_data() {
    FtlConfig config = small_config();
    Ftl ftl(config);

    // Overwrite a small hot set far past physical capacity: without GC the
    // device would run out of blocks long before this finishes.
    const std::int64_t hot_pages = 20;
    for (int round = 0; round < 100; ++round) {
        for (std::int64_t lpn = 0; lpn < hot_pages; ++lpn) {
            ftl.write(lpn);
        }
    }

    CHECK(ftl.metrics().gc_runs > 0);
    CHECK(ftl.metrics().erases > 0);
    CHECK(ftl.free_blocks() >= config.gc_free_block_threshold - 1);

    // Every logical page still resolves to a valid physical page holding it.
    for (std::int64_t lpn = 0; lpn < hot_pages; ++lpn) {
        const std::int64_t ppn = ftl.read(lpn);
        CHECK(ppn != kInvalidPpn);
        CHECK(ftl.nand().page_at(ppn).is_valid());
        CHECK_EQ(ftl.nand().page_at(ppn).lpn(), lpn);
    }
    CHECK_EQ(ftl.nand().valid_pages(), hot_pages);
}

void test_no_two_logical_pages_share_a_physical_page() {
    FtlConfig config = small_config();
    Ftl ftl(config);

    WorkloadConfig workload;
    workload.type = WorkloadType::Random;
    workload.logical_pages = ftl.logical_pages();
    workload.requests = 3000;
    WorkloadGenerator generator(workload);

    Request request;
    while (generator.next(request)) {
        ftl.write(request.lpn);
    }

    // Reverse-map every valid page: the mapping table and the NAND contents
    // must agree in both directions.
    std::map<std::int64_t, std::int64_t> owner;
    const Nand& nand = ftl.nand();
    for (int b = 0; b < nand.block_count(); ++b) {
        for (int p = 0; p < nand.pages_per_block(); ++p) {
            const Page& page = nand.block(b).page(p);
            if (!page.is_valid()) {
                continue;
            }
            const std::int64_t ppn = nand.ppn_of(b, p);
            CHECK(owner.find(page.lpn()) == owner.end());
            owner[page.lpn()] = ppn;
            CHECK_EQ(ftl.mapping().lookup(page.lpn()), ppn);
        }
    }
    CHECK_EQ(static_cast<std::int64_t>(owner.size()), ftl.mapping().mapped_pages());
}

void test_write_amplification_is_at_least_one() {
    FtlConfig config = small_config();
    Ftl ftl(config);

    // Random writes over the whole address space, so live data is scattered and
    // GC victims still hold valid pages. (A cyclic i % N pattern would be
    // sequential: it invalidates whole blocks in order and copies nothing.)
    WorkloadConfig workload;
    workload.type = WorkloadType::Random;
    workload.logical_pages = ftl.logical_pages();
    workload.requests = 5000;
    WorkloadGenerator generator(workload);

    Request request;
    while (generator.next(request)) {
        ftl.write(request.lpn);
    }

    const Metrics& metrics = ftl.metrics();
    CHECK(metrics.write_amplification() >= 1.0);
    CHECK_EQ(metrics.physical_writes(),
             metrics.host_writes + metrics.gc_writes + metrics.wl_writes);
    // Scattered invalidation must force copy-outs.
    CHECK(metrics.gc_writes > 0);
    CHECK_EQ(metrics.host_writes, workload.requests);
}

/// The flip side of the case above: a purely cyclic (sequential) overwrite
/// invalidates blocks whole, so GC should copy nothing at all.
void test_cyclic_overwrite_copies_nothing() {
    FtlConfig config = small_config();
    Ftl ftl(config);
    for (int i = 0; i < 2000; ++i) {
        ftl.write(i % 40);
    }
    const Metrics& metrics = ftl.metrics();
    CHECK(metrics.gc_runs > 0);
    CHECK_EQ(metrics.gc_writes, 0u);
    CHECK(std::abs(metrics.write_amplification() - 1.0) < 1e-9);
}

void test_sequential_workload_amplifies_less_than_random() {
    auto run = [](WorkloadType type) {
        FtlConfig config;
        config.blocks = 64;
        config.pages_per_block = 32;
        config.overprovisioning = 0.15;
        config.wear_leveling = WearLevelingMode::Dynamic;
        Ftl ftl(config);

        WorkloadConfig workload;
        workload.type = type;
        workload.logical_pages = ftl.logical_pages();
        workload.requests = 60000;
        WorkloadGenerator generator(workload);

        Request request;
        while (generator.next(request)) {
            ftl.write(request.lpn);
        }
        return ftl.metrics().write_amplification();
    };

    const double sequential = run(WorkloadType::Sequential);
    const double random = run(WorkloadType::Random);
    // Sequential overwrites invalidate whole blocks at a time, so GC copies
    // almost nothing; random writes scatter invalidation across every block.
    CHECK(sequential < random);
    CHECK(sequential < 1.2);
}

void test_cost_benefit_policy_runs_and_is_consistent() {
    auto run = [](GcPolicy policy) {
        FtlConfig config;
        config.blocks = 64;
        config.pages_per_block = 32;
        config.overprovisioning = 0.15;
        config.gc_policy = policy;
        config.wear_leveling = WearLevelingMode::Dynamic;
        Ftl ftl(config);

        WorkloadConfig workload;
        workload.type = WorkloadType::Hotspot;
        workload.logical_pages = ftl.logical_pages();
        workload.requests = 40000;
        WorkloadGenerator generator(workload);

        Request request;
        while (generator.next(request)) {
            ftl.write(request.lpn);
        }
        return ftl;
    };

    const Ftl greedy = run(GcPolicy::Greedy);
    const Ftl cost_benefit = run(GcPolicy::CostBenefit);

    CHECK(greedy.metrics().gc_runs > 0);
    CHECK(cost_benefit.metrics().gc_runs > 0);
    CHECK(greedy.metrics().write_amplification() >= 1.0);
    CHECK(cost_benefit.metrics().write_amplification() >= 1.0);
    // Both policies serve the same host writes, whatever they cost internally.
    CHECK_EQ(greedy.metrics().host_writes, cost_benefit.metrics().host_writes);
}

void test_reset_metrics_keeps_device_contents() {
    Ftl ftl(small_config());
    for (std::int64_t lpn = 0; lpn < 30; ++lpn) {
        ftl.write(lpn);
    }
    const std::int64_t before = ftl.nand().valid_pages();
    const std::int64_t ppn = ftl.read(7);

    ftl.reset_metrics();
    CHECK_EQ(ftl.metrics().host_writes, 0u);
    CHECK_EQ(ftl.metrics().erases, 0u);
    CHECK_EQ(ftl.metrics().host_reads, 0u);
    // Counters reset; the array and the mapping do not.
    CHECK_EQ(ftl.nand().valid_pages(), before);
    CHECK_EQ(ftl.read(7), ppn);
}

// --- Wear leveling --------------------------------------------------------

void test_dynamic_wear_leveling_narrows_erase_spread() {
    auto run = [](WearLevelingMode mode) {
        FtlConfig config;
        config.blocks = 64;
        config.pages_per_block = 32;
        config.overprovisioning = 0.15;
        config.wear_leveling = mode;
        Ftl ftl(config);

        WorkloadConfig workload;
        workload.type = WorkloadType::Hotspot;
        workload.logical_pages = ftl.logical_pages();
        workload.requests = 60000;
        WorkloadGenerator generator(workload);

        Request request;
        while (generator.next(request)) {
            ftl.write(request.lpn);
        }
        return compute_erase_stats(ftl.nand());
    };

    const EraseStats none = run(WearLevelingMode::None);
    const EraseStats dynamic = run(WearLevelingMode::Dynamic);
    CHECK(dynamic.stddev < none.stddev);
    CHECK(dynamic.spread() <= none.spread());
}

void test_static_wear_leveling_migrates_cold_data() {
    FtlConfig config;
    config.blocks = 64;
    config.pages_per_block = 32;
    config.overprovisioning = 0.15;
    config.wear_leveling = WearLevelingMode::Static;
    config.static_wl_interval = 500;
    config.static_wl_threshold = 2;
    Ftl ftl(config);

    // Write the whole address space once so cold data exists, then hammer a
    // narrow hot region -- the cold blocks would otherwise never be erased.
    for (std::int64_t lpn = 0; lpn < ftl.logical_pages(); ++lpn) {
        ftl.write(lpn);
    }
    for (int i = 0; i < 40000; ++i) {
        ftl.write(i % 64);
    }

    CHECK(ftl.metrics().wl_migrations > 0);
    CHECK(ftl.metrics().wl_writes > 0);
    // Migrations are relocations, so they count toward amplification.
    CHECK(ftl.metrics().physical_writes() >
          ftl.metrics().host_writes + ftl.metrics().gc_writes - 1);

    // Data survives migration.
    for (std::int64_t lpn = 0; lpn < 64; ++lpn) {
        const std::int64_t ppn = ftl.read(lpn);
        CHECK(ppn != kInvalidPpn);
        CHECK_EQ(ftl.nand().page_at(ppn).lpn(), lpn);
    }
}

// --- Metrics --------------------------------------------------------------

void test_erase_stats_on_fresh_device() {
    Nand nand(8, 4);
    const EraseStats stats = compute_erase_stats(nand);
    CHECK_EQ(stats.min, 0u);
    CHECK_EQ(stats.max, 0u);
    CHECK_EQ(stats.total, 0u);
    CHECK(std::abs(stats.mean) < 1e-9);
    CHECK(std::abs(stats.stddev) < 1e-9);
    CHECK(!format_erase_histogram(nand).empty());
}

void test_erase_stats_track_the_array() {
    Nand nand(4, 4);
    nand.erase_block(0, 1);
    nand.erase_block(0, 2);
    nand.erase_block(1, 3);

    const EraseStats stats = compute_erase_stats(nand);
    CHECK_EQ(stats.min, 0u);
    CHECK_EQ(stats.max, 2u);
    CHECK_EQ(stats.total, 3u);
    CHECK_EQ(stats.spread(), 2u);
    CHECK_EQ(nand.total_erases(), 3u);
    CHECK(std::abs(stats.mean - 0.75) < 1e-9);
}

// --- Workload generation --------------------------------------------------

void test_generators_stay_in_range_and_are_deterministic() {
    WorkloadConfig config;
    config.logical_pages = 100;
    config.requests = 500;
    config.seed = 7;

    for (const WorkloadType type :
         {WorkloadType::Sequential, WorkloadType::Random, WorkloadType::Hotspot}) {
        config.type = type;
        WorkloadGenerator first(config);
        WorkloadGenerator second(config);

        Request a;
        Request b;
        std::uint64_t count = 0;
        while (first.next(a)) {
            CHECK(second.next(b));
            CHECK_EQ(a.lpn, b.lpn);  // same seed, same stream
            CHECK(a.lpn >= 0 && a.lpn < config.logical_pages);
            ++count;
        }
        CHECK_EQ(count, config.requests);
        CHECK(!second.next(b));  // both exhausted together
    }
}

void test_sequential_generator_wraps() {
    WorkloadConfig config;
    config.type = WorkloadType::Sequential;
    config.logical_pages = 10;
    config.requests = 12;
    WorkloadGenerator generator(config);

    Request request;
    for (int i = 0; i < 12; ++i) {
        CHECK(generator.next(request));
        CHECK_EQ(request.lpn, i % 10);
    }
}

void test_hotspot_generator_is_skewed() {
    WorkloadConfig config;
    config.type = WorkloadType::Hotspot;
    config.logical_pages = 1000;
    config.requests = 10000;
    config.hot_fraction = 0.10;
    config.hot_ratio = 0.90;
    WorkloadGenerator generator(config);

    int hot_hits = 0;
    Request request;
    while (generator.next(request)) {
        if (request.lpn < 100) {
            ++hot_hits;
        }
    }
    // ~90% by construction; allow slack for sampling noise.
    CHECK(hot_hits > 8500);
    CHECK(hot_hits < 9500);
}

void test_read_ratio_produces_reads() {
    WorkloadConfig config;
    config.type = WorkloadType::Random;
    config.logical_pages = 100;
    config.requests = 2000;
    config.read_ratio = 0.5;
    WorkloadGenerator generator(config);

    int reads = 0;
    Request request;
    while (generator.next(request)) {
        if (request.op == RequestOp::Read) {
            ++reads;
        }
    }
    CHECK(reads > 800);
    CHECK(reads < 1200);
}

// --- Policy name parsing --------------------------------------------------

void test_policy_parsing_round_trip() {
    GcPolicy gc = GcPolicy::Greedy;
    CHECK(gc_policy_from_string("cost-benefit", gc));
    CHECK(gc == GcPolicy::CostBenefit);
    CHECK_EQ(to_string(gc), std::string("cost-benefit"));
    CHECK(gc_policy_from_string("greedy", gc));
    CHECK(gc == GcPolicy::Greedy);
    CHECK(!gc_policy_from_string("lru", gc));

    WearLevelingMode mode = WearLevelingMode::None;
    CHECK(wear_leveling_from_string("static", mode));
    CHECK(mode == WearLevelingMode::Static);
    CHECK_EQ(to_string(mode), std::string("static"));
    CHECK(!wear_leveling_from_string("aggressive", mode));

    WorkloadType type = WorkloadType::Random;
    CHECK(workload_type_from_string("hotspot", type));
    CHECK(type == WorkloadType::Hotspot);
    CHECK(!workload_type_from_string("zipf", type));
}

void register_tests() {
    add_test("block program/invalidate/erase", test_block_program_invalidate_erase);
    add_test("nand addressing", test_nand_addressing);
    add_test("read after write", test_read_after_write);
    add_test("out-of-place write invalidates old page",
             test_out_of_place_write_invalidates_old_page);
    add_test("write outside exported capacity is rejected", test_write_out_of_range_rejected);
    add_test("overprovisioning reserves capacity", test_overprovisioning_reserves_capacity);
    add_test("gc reclaims space and preserves data", test_gc_reclaims_space_and_preserves_data);
    add_test("mapping and nand stay consistent", test_no_two_logical_pages_share_a_physical_page);
    add_test("write amplification >= 1", test_write_amplification_is_at_least_one);
    add_test("cyclic overwrite copies nothing", test_cyclic_overwrite_copies_nothing);
    add_test("sequential amplifies less than random",
             test_sequential_workload_amplifies_less_than_random);
    add_test("cost-benefit policy is consistent", test_cost_benefit_policy_runs_and_is_consistent);
    add_test("reset_metrics keeps device contents", test_reset_metrics_keeps_device_contents);
    add_test("dynamic wear leveling narrows erase spread",
             test_dynamic_wear_leveling_narrows_erase_spread);
    add_test("static wear leveling migrates cold data",
             test_static_wear_leveling_migrates_cold_data);
    add_test("erase stats on a fresh device", test_erase_stats_on_fresh_device);
    add_test("erase stats track the array", test_erase_stats_track_the_array);
    add_test("generators stay in range and are deterministic",
             test_generators_stay_in_range_and_are_deterministic);
    add_test("sequential generator wraps", test_sequential_generator_wraps);
    add_test("hotspot generator is skewed", test_hotspot_generator_is_skewed);
    add_test("read ratio produces reads", test_read_ratio_produces_reads);
    add_test("policy name parsing", test_policy_parsing_round_trip);
}

}  // namespace

int main() {
    register_tests();

    int failed_tests = 0;
    for (const TestCase& test : registry()) {
        const int before = g_failures;
        test.body();
        const bool ok = g_failures == before;
        std::cout << (ok ? "PASS  " : "FAIL  ") << test.name << "\n";
        if (!ok) {
            ++failed_tests;
        }
    }

    std::cout << "\n"
              << registry().size() - static_cast<std::size_t>(failed_tests) << "/"
              << registry().size() << " tests passed (" << g_checks << " checks, " << g_failures
              << " failures)\n";
    return failed_tests == 0 ? 0 : 1;
}
