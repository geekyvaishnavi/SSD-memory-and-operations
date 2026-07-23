#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "ftlsim/ftl/gc.hpp"
#include "ftlsim/ftl/mapping.hpp"
#include "ftlsim/ftl/wearlevel.hpp"
#include "ftlsim/metrics/metrics.hpp"
#include "ftlsim/nand/nand.hpp"

namespace ftlsim {

struct FtlConfig {
    int blocks = 128;
    int pages_per_block = 64;
    /// Fraction of physical capacity hidden from the host. GC needs this slack
    /// to make progress; with zero over-provisioning a full device deadlocks.
    double overprovisioning = 0.10;
    /// GC runs until at least this many clean blocks are available.
    int gc_free_block_threshold = 3;
    GcPolicy gc_policy = GcPolicy::Greedy;
    WearLevelingMode wear_leveling = WearLevelingMode::Dynamic;
    /// Static wear leveling: check every N host writes, migrate when the
    /// erase-count spread exceeds the threshold.
    int static_wl_interval = 1000;
    std::uint64_t static_wl_threshold = 8;
};

/// The Flash Translation Layer: maps logical pages to physical pages, performs
/// out-of-place writes, and reclaims space through garbage collection.
class Ftl {
public:
    explicit Ftl(const FtlConfig& config);

    const FtlConfig& config() const { return config_; }
    const Nand& nand() const { return nand_; }
    const Metrics& metrics() const { return metrics_; }
    const MappingTable& mapping() const { return mapping_; }
    std::int64_t logical_pages() const { return mapping_.logical_pages(); }
    std::uint64_t timestep() const { return timestep_; }
    int open_block() const { return open_block_; }
    int free_blocks() const { return static_cast<int>(free_blocks_.size()); }

    /// Host write: program `lpn` to a fresh physical page and invalidate the
    /// previous copy. Triggers GC and wear leveling as needed.
    void write(std::int64_t lpn);
    /// Host read: returns the physical page backing `lpn`, or kInvalidPpn.
    std::int64_t read(std::int64_t lpn);

    /// Zero the counters while leaving the NAND contents alone. Used to drop a
    /// warm-up phase from the measurement, so the reported WAF describes the
    /// steady state rather than the fill.
    void reset_metrics() { metrics_ = Metrics(); }

    /// Drain the GC / wear-leveling events recorded since the last call.
    std::vector<GcEvent> take_gc_events();
    std::vector<WearLevelEvent> take_wear_level_events();

private:
    /// Reclaim blocks until the clean-block reserve is satisfied.
    void ensure_free_space();
    /// Run one GC cycle. Returns false when no victim can make progress.
    bool collect_garbage();
    void maybe_static_wear_level();

    /// Program `lpn` into the open block, opening a new one if needed.
    std::int64_t allocate_page(std::int64_t lpn);
    void open_new_block();
    void invalidate_ppn(std::int64_t ppn);

    FtlConfig config_;
    Nand nand_;
    MappingTable mapping_;
    Metrics metrics_;
    std::unique_ptr<VictimSelector> selector_;
    WearLeveler wear_leveler_;

    std::vector<int> free_blocks_;
    int open_block_ = -1;
    std::uint64_t timestep_ = 0;
    bool relocating_ = false;  // guards against re-entrant GC during copy-out

    std::vector<GcEvent> gc_events_;
    std::vector<WearLevelEvent> wl_events_;
};

}  // namespace ftlsim
